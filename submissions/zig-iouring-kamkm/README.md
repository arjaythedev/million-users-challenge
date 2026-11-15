# Zig feed server

A Linux x86-64 submission built with Zig 0.13.0: one io_uring loop per core, each driving its own PostgreSQL sessions. libpq connects, authenticates and prepares the statements; after that the loop reads and writes the session's socket itself and speaks the wire protocol directly.

This is a separate entry from `zig-epoll-kamkm` (PR #131), which runs the same design on epoll and stays as it is. The HTTP framing, JSON, token, gzip and PostgreSQL wire code are shared with that entry; what is new is how a loop waits for and performs I/O.

## How a request moves

A loop is a thread that owns everything it touches: its ring, its listening sockets (`SO_REUSEPORT` gives every loop its own accept queue on both ports), the clients it accepted and its PostgreSQL sessions. A request is received, parsed, queued, answered and sent on that one thread, so there are no locks, condition variables or thread hand-offs between a request and its response.

Accepts, receives and sends are operations in the loop's ring rather than system calls:

- One multishot accept per listener reports every new connection.
- One multishot receive per client stays in the ring for the life of the connection. When input arrives the kernel copies it into one of the loop's lent buffers (4096 buffers of 4 KiB, lent with `IORING_OP_PROVIDE_BUFFERS`), so an idle client holds no buffer. A request is parsed where it lies and the buffer goes back at the end of the pass; only a partial request, or requests sent ahead while the client waits for the database, are copied to a buffer of the client's own.
- A response is one send operation. The next request of the same client is served only after it completes, so responses stay in order.
- Each PostgreSQL session has one receive into its own buffer and at most one send in the ring.

A pass of the loop handles every completion that is ready, lets the sessions send what has been queued, then hands the kernel all new operations and waits in one `io_uring_enter`. The ring is set up with `IORING_SETUP_DEFER_TASKRUN`, so the kernel finishes this loop's network work only when the loop asks for completions. A loop that had work sleeps to the end of its pass (2 ms by default) unless 512 completions gather first; a loop that found nothing sleeps until something completes. Under load this turns many wakeups, each with a few events, into one wakeup per pass with many; in the lab it cut the server's own CPU time per request almost in half.

Closing a client shuts its socket down, which completes what the ring still holds for it; the descriptor and the record are freed only after the last of those completions. The server accepts chunked bodies and `Expect: 100-continue`, honors `Connection: close`, closes a connection whose request exceeds 16 KiB, and answers malformed or ambiguous framing with a 400 and closes.

## Database

Every request runs the SQL from `SPEC.md` as its own statement; response data and JWT verification results are never cached across requests.

Each loop keeps two queues, reads and writes. A free session takes a share of a queue and sends it as one batch: a single send of `Bind`/`Execute` pairs closed by one `Sync`. Nothing asks the server to describe a result, so no `RowDescription` comes back, and JSON is written straight from the `DataRow` bytes. Each read is answered as soon as its own `CommandComplete` arrives.

A session starts batches no closer together than its gather window (5 ms by default). An idle session sends a lone request at once, so light load pays no delay; under load the requests of one window travel together.

If a statement in a read batch fails, that read alone gets the error, and the queries the server skipped behind it are sent again. Reads on a session that is lost run once more on another session; a lost session is replaced by a helper thread, so no loop ever blocks on a connect.

Writes are verified on the loop, then committed in groups of up to 64 statements under one `Sync`, which PostgreSQL runs as one implicit transaction: one round trip and one commit for the group. Nothing is answered before `ReadyForQuery`, which the server sends only after that commit is durable. A statement error aborts the group and nothing in it commits, so each write then runs again as its own transaction; a like of a missing post becomes a 404 there. A lost connection or a timeout leaves the commit outcome unknown, so those writes return an error and are never sent again. Groups on different loops can insert the same (user, post) like concurrently; every group inserts its likes in key order, so two transactions can wait for each other's row but never in a cycle.

JWTs are verified on every authenticated request with OpenSSL HMAC-SHA256. ISA-L 2.31.1 compresses responses of at least 1,024 bytes when the client accepts gzip; compression state belongs to the loop's thread. Post bodies must be valid UTF-8; Unicode whitespace is trimmed before the 500-character limit is checked. A body with an embedded NUL is rejected, never stored truncated.

## Build and run

Linux 6.1 or later (Ubuntu 24.04 ships 6.8). `install.sh` installs Zig and the C build tools on Ubuntu. `build.sh` downloads SHA-256-pinned PostgreSQL 18.6 and ISA-L sources, links their static client libraries, and emits `feed-zig-iouring`. The PostgreSQL client uses plain TCP on the challenge's private database network. The installer reuses an existing pinned compiler, and the build rejects a different Zig version.

Set `DATABASE_URL`, `JWT_SECRET`, `HOST`, `PORT`, and optional `EXTRA_PORT`, then run `./start.sh` in the foreground. Sessions use `jit = off` and `plan_cache_mode = force_generic_plan`. A batch that is not answered within five seconds ends its session.

| Setting | Default | Meaning |
|---|---|---|
| `HTTP_LOOPS` | available CPUs | Loops (1..256). |
| `DB_POOL` | 64 | Ceiling on PostgreSQL sessions (1..512). There are never more loops than sessions; a loop with a single session uses it for reads and writes in turn. |
| `READ_SESSIONS` | 16 | Read sessions over all loops, shared out evenly and rounded up (4 per loop on 4 cores, 1 per loop on 16). |
| `WRITE_SESSIONS` | 1 | Write sessions per loop. |
| `READ_BATCH` | 64 | Most reads in one batch (1..256). Write groups hold up to 64. |
| `READ_WINDOW_US`, `WRITE_WINDOW_US` | 5000 | Gather windows in microseconds; 0 sends whatever is queued as soon as a session is free. |
| `PASS_US` | 2000 | Length of a busy loop's pass in microseconds; 0 wakes on every completion. |
| `PASS_EVENTS` | 512 | Completions that end a pass early. |

With the defaults the server opens 20 sessions on 4 cores, 24 on 8 and 32 on 16.

## Validation

Run the repository API suite against a fresh seed clone, then the regressions:

```sh
bash test/test.sh http://127.0.0.1:3000
python3 submissions/zig-iouring-kamkm/tests/request_buffers.py http://127.0.0.1:3000
python3 submissions/zig-iouring-kamkm/tests/feed_freshness.py http://127.0.0.1:3000 --database feed_test
python3 submissions/zig-iouring-kamkm/tests/concurrent_writes.py http://127.0.0.1:3000 --database feed_test
python3 submissions/zig-iouring-kamkm/tests/contract_edges.py http://127.0.0.1:3000
python3 submissions/zig-iouring-kamkm/tests/http_transport.py http://127.0.0.1:3000 --idle 16
python3 submissions/zig-iouring-kamkm/tests/gzip_responses.py http://127.0.0.1:3000
```

The Python probes use the usual `psql` connection environment. Add `--docker CONTAINER` when PostgreSQL runs in Docker. Repeat the suite with `DB_POOL=1`.

## Measurements

See [BENCHMARKS.md](BENCHMARKS.md) for lab measurements next to `zig-epoll-kamkm`. These development results are not official leaderboard scores.

Front: direct
