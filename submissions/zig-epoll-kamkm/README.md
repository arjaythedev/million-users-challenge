# Zig feed server

A Linux x86-64 submission built with Zig 0.13.0: one epoll loop per core, each driving its own PostgreSQL sessions. libpq connects, authenticates and prepares the statements; after that the loop reads and writes the session's socket itself and speaks the wire protocol directly.

This is a separate entry from `zig-kamkm` (PR #43), which hands requests to blocking libpq threads and stays as it was scored. The HTTP framing, JSON, token and gzip code are shared with that entry; the event loop and everything that talks to PostgreSQL are new.

## How a request moves

A loop is a thread that owns everything it touches: its listening sockets (`SO_REUSEPORT` gives every loop its own accept queue on both ports), the clients it accepted and its PostgreSQL sessions. A request is read, parsed, queued, answered and written on that one thread, so there are no locks, condition variables or thread hand-offs between a request and its response.

`zig-kamkm` runs two event loops that pass every request to one of 16 database threads through a mutex-guarded queue and receive the response back through an eventfd. Each of those threads makes blocking libpq calls, and under steady load most requests travel to PostgreSQL alone. That is the reason for this design.

An idle client is a record of under 100 bytes. Sockets are edge-triggered and registered once; a request costs one `read` and one `send`, with no `epoll_ctl`. A client waiting for the database is not read, so requests it sends ahead stay in the socket and are served in order after the response. Input is read into a buffer the loop owns; only a partial request, or requests sent ahead, are copied to a buffer of the client's own, which is freed when it empties. The server accepts chunked bodies and `Expect: 100-continue`, honors `Connection: close`, closes a connection whose request exceeds 16 KiB, and answers malformed or ambiguous framing with a 400 and closes. Responses carry no `Vary` header, and a `Connection` header only to close the connection or to keep an HTTP/1.0 client's open: HTTP/1.1 keeps connections open by default.

## Database

Every request runs the SQL from `SPEC.md` as its own statement; response data and JWT verification results are never cached across requests.

Each loop keeps two queues, reads and writes. A free session takes a share of a queue and sends it as one batch: a single `send` of `Bind`/`Execute` pairs closed by one `Sync`. Nothing asks the server to describe a result, so no `RowDescription` comes back, and JSON is written straight from the `DataRow` bytes. Results come as text except `created_at`, which `Bind` asks for as an eight-byte binary timestamp: 21 bytes less per post than its text form, turned into ISO 8601 with integer date arithmetic. Each read is answered as soon as its own `CommandComplete` arrives.

A session starts batches no closer together than its gather window (5 ms by default). An idle session sends a lone request at once, so light load pays no delay; under load the requests of one window travel together. At 600,000 users in the lab, `zig-kamkm` exchanged 2.08 packets with PostgreSQL per request and this one 0.63.

If a statement in a read batch fails, that read alone gets the error, and the queries the server skipped behind it are sent again. Reads on a session that is lost run once more on another session; a lost session is replaced immediately, by a helper thread, so no loop ever blocks on a connect.

Writes are verified on the loop, then committed in groups of up to 64 statements under one `Sync`, which PostgreSQL runs as one implicit transaction: one round trip and one commit for the group. Nothing is answered before `ReadyForQuery`, which the server sends only after that commit is durable. A statement error aborts the group and nothing in it commits, so each write then runs again as its own transaction; a like of a missing post becomes a 404 there. A lost connection or a timeout leaves the commit outcome unknown, so those writes return an error and are never sent again. Groups on different loops can insert the same (user, post) like concurrently; every group inserts its likes in key order, so two transactions can wait for each other's row but never in a cycle.

JWTs are verified on every authenticated request with OpenSSL HMAC-SHA256. ISA-L 2.31.1 compresses responses of at least 1,024 bytes when the client accepts gzip; compression state belongs to the loop's thread. Post bodies must be valid UTF-8; Unicode whitespace is trimmed before the 500-character limit is checked. A body with an embedded NUL is rejected, never stored truncated.

## Build and run

`install.sh` installs Zig and the C build tools on Ubuntu. `build.sh` downloads SHA-256-pinned PostgreSQL 18.6 and ISA-L sources, links their static client libraries, and emits `feed-zig`. The PostgreSQL client uses plain TCP on the challenge's private database network. The installer reuses an existing pinned compiler, and the build rejects a different Zig version.

Set `DATABASE_URL`, `JWT_SECRET`, `HOST`, `PORT`, and optional `EXTRA_PORT`, then run `./start.sh` in the foreground. Sessions use `jit = off` and `plan_cache_mode = force_generic_plan`. A batch that is not answered within five seconds ends its session.

| Setting | Default | Meaning |
|---|---|---|
| `HTTP_LOOPS` | available CPUs | Event loops (1..256). |
| `DB_POOL` | 64 | Ceiling on PostgreSQL sessions (1..512). There are never more loops than sessions; a loop with a single session uses it for reads and writes in turn. |
| `READ_SESSIONS` | 16 | Read sessions over all loops, shared out evenly and rounded up (4 per loop on 4 cores, 1 per loop on 16). |
| `WRITE_SESSIONS` | 1 | Write sessions per loop. |
| `READ_BATCH` | 64 | Most reads in one batch (1..256). Write groups hold up to 64. |
| `READ_WINDOW_US`, `WRITE_WINDOW_US` | 5000 | Gather windows in microseconds; 0 sends whatever is queued as soon as a session is free. |

With the defaults the server opens 20 sessions on 4 cores, 24 on 8 and 32 on 16.

## Validation

Run the repository API suite against a fresh seed clone, then the regressions:

```sh
bash test/test.sh http://127.0.0.1:3000
python3 submissions/zig-epoll-kamkm/tests/request_buffers.py http://127.0.0.1:3000
python3 submissions/zig-epoll-kamkm/tests/feed_freshness.py http://127.0.0.1:3000 --database feed_test
python3 submissions/zig-epoll-kamkm/tests/concurrent_writes.py http://127.0.0.1:3000 --database feed_test
python3 submissions/zig-epoll-kamkm/tests/contract_edges.py http://127.0.0.1:3000
python3 submissions/zig-epoll-kamkm/tests/http_transport.py http://127.0.0.1:3000 --idle 16
python3 submissions/zig-epoll-kamkm/tests/gzip_responses.py http://127.0.0.1:3000
```

The Python probes use the usual `psql` connection environment. Add `--docker CONTAINER` when PostgreSQL runs in Docker. Repeat the suite with `DB_POOL=1`.

To exercise partial sends and retryable send failures, compile `tests/send_faults.c` with `cc -shared -fPIC -std=c11 -Wall -Wextra -Werror tests/send_faults.c -ldl -o /tmp/zig-send-faults.so`. Start a test server with `LD_PRELOAD=/tmp/zig-send-faults.so` and rerun the Python suites. The shim limits nonblocking sends and returns `EAGAIN` on every seventh call, on client sockets and on the PostgreSQL sessions alike.

## Measurements

See [BENCHMARKS.md](BENCHMARKS.md) for lab measurements of this design next to `zig-kamkm`. These development results are not official leaderboard scores.

Front: direct
