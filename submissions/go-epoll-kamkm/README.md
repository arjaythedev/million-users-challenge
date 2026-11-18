# Go feed server

A Go 1.27.1 submission: one epoll event loop per core, written against the system calls directly (`golang.org/x/sys/unix`), with each loop driving its own PostgreSQL sessions. pgx's `pgconn` opens and authenticates the sessions and prepares the statements; after that the server speaks the PostgreSQL wire protocol itself.

This is a separate entry from `go-kamkm` (PR #41), which is built on gnet and a pgx pool and stays as it was scored. Nothing of gnet or the pool remains here; the request parser and the JSON encoder started from that entry's.

## How a request moves

A loop is a thread that owns everything it touches: its listening sockets (`SO_REUSEPORT` gives every loop its own accept queue on both ports), the client connections it accepted and its PostgreSQL sessions. A request is read, parsed, queued, answered and written on that one thread, so there are no locks, channels or scheduler hand-offs between a request and its response, and no allocation per read request. Sockets are read and written with `recvfrom` and `sendto`, which go straight to the socket layer and skip the generic file checks that `read` and `write` make on every call.

`go-kamkm` passes requests between event-loop goroutines, database goroutines and responder goroutines. With every processor held by an event loop, the others run only when the runtime preempts a loop, which showed up on the rig as a median latency of tens of milliseconds long before the CPU was full. That is the reason for this design.

Connection state is a 24-byte record in a table indexed by file descriptor, not a heap object, so a million idle connections are a few dozen MiB that the garbage collector never scans. Queued work and deadlines name a connection by descriptor and generation, so a reply for a connection that has gone cannot reach a later one that reuses the number. Client sockets are edge-triggered: a connection waiting for the database is not read, and input that arrives meanwhile (requests sent ahead) is served in order afterwards.

A fast parser handles ordinary HTTP/1.1 requests; Go's standard HTTP parser handles chunked bodies, `Expect`, encoded targets and other request shapes. Idle connections have a 180-second timeout and a partial request has 15 seconds from its first byte, enforced by a per-loop timing wheel with one-second slots. Requests only move a connection's deadline, and a sweep files it again, so an active connection is examined about once per idle timeout.

## Database

Every request runs the SQL from `SPEC.md` as its own statement; response data and JWT verification results are never cached across requests.

Each loop keeps two queues, reads and writes. A batch is a single `write` of `Bind`/`Execute` pairs closed by one `Sync`. Nothing asks the server to describe a result, so no `RowDescription` comes back; `Bind` asks for numbers as text and `created_at` as a binary timestamp, and JSON is written straight from the `DataRow` bytes. Each read is answered as soon as its own rows have arrived.

A loop starts at most one read batch and one write batch per gather window (3 ms by default), and the batch takes everything queued, up to the batch limit, to one free session. Only a full batch goes out before the window ends; an idle loop sends a lone request at once, so light load pays no delay. Under load a window's requests share one send, one reply stream and its ACKs: at 600,000 users in the lab, this exchanges 0.41 packets with PostgreSQL per request (0.69 when every free session took a share of the queue, 1.09 for `go-kamkm`). The other sessions start work only while earlier batches are still out, so there are many of them: a request that waits for a free session past 40 ms costs the network two extra packets (below).

If a statement in a read batch fails, that read alone gets the error and the queries the server skipped behind it are sent again. Reads on a session that is lost are sent once more on another session; a lost session is replaced immediately.

Writes are verified on the loop, then committed in groups of up to 64 statements under one `Sync`, which PostgreSQL runs as one implicit transaction: one round trip and one commit for the group. Nothing is answered before `ReadyForQuery`, which the server sends only after that commit is durable. A statement error aborts the group and nothing in it commits, so each write then runs again as its own transaction (a `Sync` after each); a like of a missing post becomes a 404 there. A lost connection or a timeout can leave the commit outcome unknown, so those writes return an error and are never replayed. Groups from different loops can insert the same (user, post) like concurrently; every group inserts its likes in key order, so two transactions can wait for each other's row but never in a cycle.

## Packets per request

On the client side every server sends about the same packets: a response per request and a reply to each of the client's TCP keep-alive probes. What a server decides is its PostgreSQL traffic (above) and whether a response also carries the ACK of its request. It does when it leaves within the kernel's delayed-ACK time of 40 ms; a slower response makes the kernel send that ACK alone, and the client's next request is then acknowledged on its own as well. The many read sessions keep requests from queueing that long for a free session. Responses carry no `Vary` or `Connection: keep-alive` header; HTTP/1.1 keeps the connection open by default.

With 820,000 users all over the network on four cores in the lab, this server sent 1.04 ACKs per request besides its responses, most of them replies to keep-alive probes, and 187,000 packets per second in all; the leading entry sent 1.04 and 188,000. At the app box's packet limit (1,100,000 users), where database round trips slow down, the extra read sessions took the ACKs from 1.25 to about 1.0 per request (the leading entry: 0.94).

Setting `TCP_QUICKACK` to 0, so that a connection is in delayed-ACK mode from its first request and again after a slow response, was measured twice and is not used. At the network limit it saved 0.01 packets per request. At the CPU limit many responses take longer than 40 ms, and each of them then needs a timer to send its ACK, which costs more than the immediate ACK it replaces: 950,000 users on two cores failed with it (p99 1.3 s) and pass without it (p99 0.2 s).

## Tokens, JSON and gzip

The application checks each token's HS256 signature, expiration and payload. Pooled HMAC states retain the key schedule, never token verification results. JSON encoding preserves the API's key order, escapes strings (scanning eight bytes at a time for the few that need it) and truncates UTC timestamps to milliseconds, converting days to a calendar date with integer arithmetic.

Bodies of 1,024 bytes or more are gzipped when the client accepts it, with ISA-L 2.31.1 at level 0 (`gzip_isal.go`): a stateless deflate per response into the loop's buffer, with compression state kept per thread on the C side, so every loop has its own. ISA-L's hand-written assembly compresses a feed response in about half the time of the portable encoder (`gzipfast.go`, one fixed-Huffman block with greedy matches), for about 20% more bytes; the response still fits one packet. Without the `isal` build tag the server uses the portable encoder instead. Smaller bodies are sent directly.

## Build and run

`install.sh` installs Go 1.27.1 and, if missing, `gcc`, `make` and `nasm` on Ubuntu. `build.sh` pins that toolchain, downloads ISA-L 2.31.1 (checked against its SHA-256) and builds it as a static library, downloads the checked-in module versions, and emits `bin/feed-go-epoll-kamkm` with the `isal` tag; the binary links only libc dynamically. `start.sh` runs it in the foreground and defaults `GOMEMLIMIT` to a fixed `24GiB` and `GOGC` to 400; explicit environment values take precedence. The script does not inspect `/proc/meminfo`.

Set `DATABASE_URL`, `JWT_SECRET`, `HOST`, `PORT`, and optional `EXTRA_PORT`. Sessions use `jit=off` and `plan_cache_mode=force_generic_plan`, and plain TCP: the loops read and write the sockets themselves, so `sslmode=disable` and the default `sslmode=prefer` connect without TLS, and a connection string that requires TLS is refused at startup. Each database batch has a five-second timeout.

| Setting | Default | Meaning |
|---|---|---|
| `HTTP_LOOPS` | `GOMAXPROCS` | Event loops (1..256). `GOMAXPROCS` is set to one more than this unless it is given, so the garbage collector does not have to preempt a loop. |
| `DB_POOL` | 128 | Ceiling on PostgreSQL sessions (1..512). There are never more loops than sessions; a loop with a single session uses it for reads and writes. |
| `READ_SESSIONS` | 64 | Read sessions over all loops, shared out evenly and rounded up (16 per loop on 4 cores, 4 per loop on 16). |
| `WRITE_SESSIONS` | 2 | Write sessions per loop; the second commits only while the first group is still out. |
| `READ_BATCH` | 64 | Most reads in one batch (1..256). Write groups hold up to 64. |
| `READ_WINDOW_US`, `WRITE_WINDOW_US` | 3000 | Gather windows in microseconds: the least time between two batches a loop starts, unless a full batch is waiting; 0 sends whatever is queued as soon as a session is free. |
| `LOOP_TRACE` | 0 | 1 logs, once a second, a loop pass over 20 ms or a database round trip over 50 ms. |

With the defaults the server opens 68 sessions on 2 cores, 72 on 4, 80 on 8 and 96 on 16.

## Validation

Run the repository API suite against a fresh seed clone, then the regressions:

```sh
bash test/run.sh submissions/go-epoll-kamkm
cd submissions/go-epoll-kamkm && CGO_ENABLED=1 go test -race . && go vet .
# The same with ISA-L, after build.sh:
export CGO_CFLAGS="-I$PWD/deps/isa-l-2.31.1/include" CGO_LDFLAGS="$PWD/deps/isa-l-2.31.1/bin/isa-l.a"
CGO_ENABLED=1 go test -race -tags isal .
# With the server running:
python3 tests/feed_freshness.py http://127.0.0.1:3000 --database feed_test
python3 tests/concurrent_writes.py http://127.0.0.1:3000 --database feed_test
python3 tests/http_transport.py http://127.0.0.1:3000 --idle 16
```

The database probes use the usual `psql` connection environment, or `--docker CONTAINER`. Unit regressions compare the fast parser with `net/http`, check JSON and timestamps against the standard library, round-trip gzip through `compress/gzip` with both encoders, reject malformed framing, and require a string token username.

`loop_test.go` runs a real event loop against scripted peers, with socket pairs standing in for the clients and for PostgreSQL: replies cut into fragments, a failed statement inside a read batch, a failed write group that must run again one statement at a time, and a lost session (reads run again, writes never do).

## Measurements

See [BENCHMARKS.md](BENCHMARKS.md) for lab measurements of this design next to `go-kamkm`. These development results are not official leaderboard scores.

Front: direct
