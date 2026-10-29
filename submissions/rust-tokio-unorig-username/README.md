# rust-tokio: thread per core, hand-written HTTP/1.1, pipelined tokio-postgres

| | |
|---|---|
| Language | Rust 1.94.0 (edition 2024), rustup in `/opt/twelve-rust` like the repo's Rust example |
| Framework / IO | none; tokio 1.54.1 current-thread runtimes (one per core), raw `TcpStream` |
| Driver | tokio-postgres 0.7.18, prepared statements, pipelined, no TLS |
| Front | direct (ports `PORT` and `EXTRA_PORT`, no Nginx) |
| Other crates | hmac 0.12.1 + sha2 0.10.9 (HS256), libdeflater 1.26.1 (gzip, libdeflate built from the crate's vendored source), memchr 2.8.3 |

All versions are pinned in `Cargo.toml` (`=x.y.z`) and `Cargo.lock` (`cargo build --locked`).

## Running

```bash
sudo bash install.sh   # build-essential, curl, Rust 1.94.0 (idempotent)
bash build.sh          # target/release/feed, -C target-cpu=native, fat LTO
DATABASE_URL=postgres://feed:pw@db:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

## Design

- **One thread per core** (`available_parallelism`, so it follows the CPU affinity mask when run on 8 or 4
  cores). Each thread runs its own current-thread tokio runtime and `LocalSet`, binds its own `SO_REUSEPORT`
  listener on both ports (backlog 65535) and owns its share of the Postgres connections. The kernel spreads new connections
  over the threads; nothing is shared between threads except the read-only config, so there are no locks or
  atomics on the request path. There are 96 read and 32 write connections in total whatever the core count
  (16 cores: 6 + 2 per thread; 4 cores: 24 + 8; limit 512): each autocommit INSERT waits for its own WAL flush, so
  fewer cores must not mean fewer commits in flight.
- **Postgres**: exactly the SPEC's statements, one per request, prepared once per connection. A read goes to the
  thread's first live read connection with fewer than 16 requests in flight, so a batch packs onto a few
  connections and tokio-postgres sends every query queued on one in one write and reads their results in one go;
  only when all are that full does it take the least loaded. A write goes to the least loaded write connection,
  so commits (one WAL flush each) run side by side. Reads and writes use separate
  connections, so a read never waits behind a commit. Writes autocommit (`synchronous_commit` untouched) and the response is sent only after the
  INSERT returns, so it is committed. A like on a missing post is SQLSTATE 23503 -> 404. Timestamps are read
  in binary (microseconds since 2000) and formatted in the app as UTC with milliseconds truncated.
- **Batching window**: a task on each thread yields to everything else that is ready and, if that whole turn
  took under 200 us (a lightly loaded thread), sleeps the thread for 2 ms. The sockets that became ready
  meanwhile are then served by one `epoll_wait`, and their queries share a Postgres round trip, instead of a
  wake-up per request and per result. A busy thread never sleeps. The cost is a few ms of latency (p95
  0.6 ms -> 8 ms locally, against the 500 ms limit).
- **Reconnect**: when a Postgres connection ends, its in-flight requests answer 500 (a write is never
  retried: its commit is uncertain) and its slot is skipped by the round-robin. A task per slot reconnects it,
  at most one try a second with a 3 s timeout, and prepares the statements again. With no live connection on
  a thread, its requests answer 500 and `/health` 503 at once. Measured under light load: killing all 96
  backends with `pg_terminate_backend` failed 4 requests and one `/health`, all in the same second; stopping
  Postgres for 11 s gave 500/503 while it was down and 200 again 2 s after it started.
- **HTTP/1.1** is parsed by hand: request line, `Content-Length`, `Authorization`, `Accept-Encoding`,
  `Connection`, `Expect: 100-continue`; `Transfer-Encoding` closes the connection (not supported). Keep-alive
  by default, `Connection: close` and HTTP/1.0 close after the response, pipelined requests are answered in
  order. There is no idle timeout: a keep-alive connection stays open until the client closes it.
- **JWT**: HS256 HMAC checked in constant time over every token (key schedule computed once, then cloned per
  request), header must be valid JSON with `alg` exactly `HS256`, payload must be valid JSON, `exp` is
  required and must be in the future, `nbf` is honored if present, `sub` must be a string of digits > 0 and
  `username` a string. No verification is remembered between requests.
- **JSON input** is validated strictly (RFC 8259: full document, escapes, surrogate pairs, UTF-8); the last
  duplicate key wins as in `JSON.parse`. The post body is trimmed with JavaScript's `String#trim` whitespace
  set and limited to 500 code points (what the Node reference and Postgres `char_length` count).
- **Output** is built with plain byte appends in the SPEC's key order; strings are escaped (`"`, `\`, control
  characters). Responses of 1 KB or more (the feed) are gzipped with libdeflate level 1 when the request sends
  `Accept-Encoding: gzip`: the 5.1 KB feed goes out as about 740 bytes.

## Memory per connection

The target is a million idle keep-alive connections, so an idle connection owns no buffer:

- Each thread has one 64 KB read buffer, one body buffer and one output buffer (thread-local). A connection
  waits for readability, reads into the thread's buffer, parses and validates the request there, and copies
  out only what must survive the database await (a post id, or user id + username + body for a write).
  Responses are built and written in the same synchronous step. Only a partial request, a pipelined
  remainder or a response the socket could not take at once gets a per-connection `Vec`, freed afterwards.
- Every await except the idle read is boxed, so the database and slow-write futures exist only while a
  request is in flight, and the idle read uses `poll_read_ready` (the waker lives in the socket's own slot)
  instead of a 152-byte `readable()` future. The connection future is 264 bytes (488 before these two changes).
- Measured: **0.88 KB of server RSS per idle connection** (8 MB baseline, 170 MB at 184,676 open connections
  that each made one request), so a million connections need about 0.9 GB of user memory plus the kernel's
  socket memory.

## Measurements (local, not the benchmark boxes)

macOS host, Docker in a 12 vCPU VM running amd64 under Rosetta, shared with other benchmarks that kept its
load average between 12 and 90 during these runs, so latencies are noisy and CPU numbers are pessimistic.
Postgres 18.6 in its own container with `box/postgresql.conf` (shared_buffers lowered to 2 GB), full seed.
Load generator: the repo's `bench/mload` over loopback (7 target IPs x 2 ports), `-gzip -think-scale 2.5`.
Server pinned to 4 cores (`taskset -c 0-3`: 4 threads, 32 Postgres connections), load generator to the other 8.

- `test/run.sh`: `passed=42 failed=0` in a clean container that ran `install.sh`, and again with `PORT` and
  `EXTRA_PORT` swapped.
- Before the batching window: mload, 300,000 users (the most the VM held reliably; 500,000 connected fine but the load generator and VM
  could not keep up): `RESULT users=300000 req/s=10338 p95=44.0ms p99=384.9ms err=0.000% pass=true`, about
  12,500 req/s during the hold, **server RSS 277 MB at 300,000 connections**, server CPU 54 s user+sys per
  60 s, i.e. **0.07 cores per 1,000 req/s** (0.04 in a quieter run), including the loopback TCP work.
  A million users is about 42,000 req/s, or roughly 3 cores at this rate.
- Batching window and least-loaded connection A/B (`test/run.sh` 42/42 with both): mload over loopback,
  server on 4 cores, load generator on 8 others, 60,000 users, `-gzip`, 90 s hold, server CPU per request
  in the hold (user + sys):
  - `-think-scale 0.5`, ~12,700 req/s (about the per-thread rate of 1M users on 16 cores): 62 -> 35 us per
    request (two or three runs each; the pause alone gives 36, the least-loaded pick alone 61.5), p95
    0.6 -> 8.1 ms, p99 0.9 -> 9.3 ms. RSS unchanged (66 MB at 60,000 connections).
  - `-think-scale 0.18`, ~35,200 req/s (1M users on 4 to 5 cores): 2.07 -> 0.87 cores, 58 -> 25 us per
    request, p95 1.0 -> 7.3 ms, p99 14 to 639 -> 9.6 ms.
  - `-think-scale 0.1`, ~62,500 req/s (near the VM's limit): 2.21 -> 1.42 cores, the same throughput, p95
    54 -> 21 ms: a busy thread does not pause, so the window does not cap throughput.
- Before the batching window: wrk, 256 connections, 4 server cores, 15 s each: `/feed` (gzip) 15,300 req/s, 41 us CPU per request;
  `/posts/:id` 23,500 req/s, 21 us; likes 3,900 req/s, 51 us. All three were limited by the shared Postgres
  container (the server used about half a core), not by the server.

## Judgment calls

- `exp` is required (rule 9 says it is checked on every request); a token without `exp` is rejected.
- Chunked request bodies and requests over 64 KB close the connection (bodies over 16 KB are out of scope).
- Unknown methods on known paths answer 404 (out of scope in the SPEC).
