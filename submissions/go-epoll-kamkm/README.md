# Go feed server

A Go 1.27.1 submission: one epoll event loop per core, written against the system calls directly (`golang.org/x/sys/unix`), with each loop driving its own PostgreSQL sessions. pgx's `pgconn` opens and authenticates the sessions and prepares the statements; after that the server speaks the PostgreSQL wire protocol itself.

This is a separate entry from `go-kamkm` (PR #41), which is built on gnet and a pgx pool and stays as it was scored. Nothing of gnet or the pool remains here; the request parser, the JSON encoder and the gzip encoder are shared with that entry.

## How a request moves

A loop is a thread that owns everything it touches: its listening sockets (`SO_REUSEPORT` gives every loop its own accept queue on both ports), the client connections it accepted and its PostgreSQL sessions. A request is read, parsed, queued, answered and written on that one thread, so there are no locks, channels or scheduler hand-offs between a request and its response, and no allocation per read request.

`go-kamkm` passes requests between event-loop goroutines, database goroutines and responder goroutines. With every processor held by an event loop, the others run only when the runtime preempts a loop, which showed up on the rig as a median latency of tens of milliseconds long before the CPU was full. That is the reason for this design.

Connection state is a 24-byte record in a table indexed by file descriptor, not a heap object, so a million idle connections are a few dozen MiB that the garbage collector never scans. Queued work and deadlines name a connection by descriptor and generation, so a reply for a connection that has gone cannot reach a later one that reuses the number. Client sockets are edge-triggered: a connection waiting for the database is not read, and input that arrives meanwhile (requests sent ahead) is served in order afterwards.

A fast parser handles ordinary HTTP/1.1 requests; Go's standard HTTP parser handles chunked bodies, `Expect`, encoded targets and other request shapes. Idle connections have a 180-second timeout and a partial request has 15 seconds from its first byte, enforced by a per-loop timing wheel with one-second slots. Requests only move a connection's deadline, and a sweep files it again, so an active connection is examined about once per idle timeout.

## Database

Every request runs the SQL from `SPEC.md` as its own statement; response data and JWT verification results are never cached across requests.

Each loop keeps two queues, reads and writes. A free session takes a share of a queue and sends it as one batch: a single `write` of `Bind`/`Execute` pairs closed by one `Sync`. Nothing asks the server to describe a result, so no `RowDescription` comes back; `Bind` asks for numbers as text and `created_at` as a binary timestamp, and JSON is written straight from the `DataRow` bytes. Each read is answered as soon as its own rows have arrived.

A session starts batches no closer together than its gather window (5 ms by default). An idle session sends a lone request at once, so light load pays no delay; under load the requests of one window travel together. That keeps the cost per request low on both machines: at 600,000 users in the lab, `go-kamkm` exchanged 1.09 packets with PostgreSQL per request and this one 0.60.

If a statement in a read batch fails, that read alone gets the error and the queries the server skipped behind it are sent again. Reads on a session that is lost are sent once more on another session; a lost session is replaced immediately.

Writes are verified on the loop, then committed in groups of up to 64 statements under one `Sync`, which PostgreSQL runs as one implicit transaction: one round trip and one commit for the group. Nothing is answered before `ReadyForQuery`, which the server sends only after that commit is durable. A statement error aborts the group and nothing in it commits, so each write then runs again as its own transaction (a `Sync` after each); a like of a missing post becomes a 404 there. A lost connection or a timeout can leave the commit outcome unknown, so those writes return an error and are never replayed. Groups from different loops can insert the same (user, post) like concurrently; every group inserts its likes in key order, so two transactions can wait for each other's row but never in a cycle.

## Tokens, JSON and gzip

The application checks each token's HS256 signature, expiration and payload. Pooled HMAC states retain the key schedule, never token verification results. JSON encoding preserves the API's key order, escapes strings and truncates UTC timestamps to milliseconds.

Bodies of 1,024 bytes or more are gzipped when the client accepts it, by a small built-in encoder (`gzipfast.go`): one DEFLATE block with the fixed Huffman codes of RFC 1951, greedy LZ77 matches from a single-probe hash table, and the standard CRC-32 trailer. For few-KiB JSON bodies a general-purpose compressor spends most of its time building Huffman tables per response; the fixed codes need none, which halves the cost of a feed response for about 17% more bytes. Smaller bodies are sent directly.

## Build and run

`install.sh` installs Go 1.27.1 on Ubuntu. `build.sh` pins that toolchain, downloads the checked-in module versions, and emits `bin/feed-go-epoll-kamkm` with cgo disabled. `start.sh` runs it in the foreground and defaults `GOMEMLIMIT` to a fixed `24GiB` and `GOGC` to 400; explicit environment values take precedence. The script does not inspect `/proc/meminfo`.

Set `DATABASE_URL`, `JWT_SECRET`, `HOST`, `PORT`, and optional `EXTRA_PORT`. Sessions use `jit=off` and `plan_cache_mode=force_generic_plan`, and plain TCP: the loops read and write the sockets themselves, so `sslmode=disable` and the default `sslmode=prefer` connect without TLS, and a connection string that requires TLS is refused at startup. Each database batch has a five-second timeout.

| Setting | Default | Meaning |
|---|---|---|
| `HTTP_LOOPS` | `GOMAXPROCS` | Event loops (1..256). `GOMAXPROCS` is set to one more than this unless it is given, so the garbage collector does not have to preempt a loop. |
| `DB_POOL` | 64 | Ceiling on PostgreSQL sessions (1..512). There are never more loops than sessions; a loop with a single session uses it for reads and writes. |
| `READ_SESSIONS` | 16 | Read sessions over all loops, shared out evenly and rounded up (4 per loop on 4 cores, 1 per loop on 16). |
| `WRITE_SESSIONS` | 1 | Write sessions per loop. |
| `READ_BATCH` | 64 | Most reads in one batch (1..256). Write groups hold up to 64. |
| `READ_WINDOW_US`, `WRITE_WINDOW_US` | 5000 | Gather windows in microseconds; 0 sends whatever is queued as soon as a session is free. |
| `LOOP_TRACE` | 0 | 1 logs, once a second, a loop pass over 20 ms or a database round trip over 50 ms. |

With the defaults the server opens 20 sessions on 4 cores, 24 on 8 and 32 on 16.

## Validation

Run the repository API suite against a fresh seed clone, then the regressions:

```sh
bash test/run.sh submissions/go-epoll-kamkm
cd submissions/go-epoll-kamkm && CGO_ENABLED=1 go test -race . && go vet .
# With the server running:
python3 tests/feed_freshness.py http://127.0.0.1:3000 --database feed_test
python3 tests/concurrent_writes.py http://127.0.0.1:3000 --database feed_test
python3 tests/http_transport.py http://127.0.0.1:3000 --idle 16
```

The database probes use the usual `psql` connection environment, or `--docker CONTAINER`. Unit regressions compare the fast parser with `net/http`, check JSON and timestamps against the standard library, reject malformed framing, and require a string token username.

`loop_test.go` runs a real event loop against scripted peers, with socket pairs standing in for the clients and for PostgreSQL: replies cut into fragments, a failed statement inside a read batch, a failed write group that must run again one statement at a time, and a lost session (reads run again, writes never do).

## Measurements

See [BENCHMARKS.md](BENCHMARKS.md) for lab measurements of this design next to `go-kamkm`. These development results are not official leaderboard scores.

Front: direct
