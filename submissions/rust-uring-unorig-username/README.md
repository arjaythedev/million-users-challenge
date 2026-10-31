# rust-uring: thread per core, raw io_uring, hand-written Postgres wire client

| | |
|---|---|
| Language | Rust 1.94.0 (edition 2024), rustup in `/opt/twelve-rust` like the repo's Rust example |
| Framework / IO | none; `io-uring` 0.7.15 (the kernel interface only, no runtime), one ring per thread |
| Driver | none: about 390 lines in `src/pg.rs` speak the Postgres v3 protocol (named prepared statements, Bind/Execute/Sync, binary results), trust, cleartext or SCRAM-SHA-256 auth, no TLS |
| Front | direct (ports `PORT` and `EXTRA_PORT`, no Nginx) |
| Other crates | libc 0.2.177, hmac 0.12.1 + sha2 0.10.9 (HS256 and SCRAM), libdeflater 1.26.1 (gzip, libdeflate built from the crate's vendored source), memchr 2.8.3 |

All versions are pinned in `Cargo.toml` (`=x.y.z`) and `Cargo.lock` (`cargo build --locked`, crates.io checksums).
Needs Linux 6.1 or newer with io_uring enabled (Ubuntu 24.04 is 6.8).

## Running

```bash
sudo bash install.sh   # build-essential, curl, Rust 1.94.0 (idempotent)
bash build.sh          # target/release/feed, -C target-cpu=native, fat LTO
DATABASE_URL=postgres://feed:pw@db:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

## Design

- **One thread per core** (`available_parallelism`, so it follows the CPU affinity mask on 8 or 4 cores). Each
  thread owns one io_uring (`SINGLE_ISSUER`, `DEFER_TASKRUN`), its own `SO_REUSEPORT` listeners on both ports
  and its share of 96 read + 32 write Postgres connections (the totals do not depend on the core count:
  each autocommit INSERT waits for its own WAL flush). Nothing is shared between threads but the read-only config.
- **Accept and receive**: one multishot accept per listener puts new sockets straight into the ring's
  registered file table (no file descriptor, no per-operation fd lookup). Each connection has one multishot
  receive that draws from a per-thread ring of 4,096 provided 2 KB buffers (also the most requests one pass reads), so an idle connection owns no
  receive buffer: the kernel picks a buffer only when bytes arrive, and the request is parsed in place and the
  buffer handed back at once.
- **Per-connection state is 16 bytes** (generation, state, flags, and a boxed buffer that exists only while a
  partial or pipelined request waits).
- **Limits**: the registered file table is 1,048,576 entries per thread (the kernel's maximum), so 4 threads
  hold up to about 4M connections; the slot field in a completion's tag is 24 bits.
- **One wait per batch**: after a pass that did work, the thread waits for 128 completions or 2 ms, whichever
  comes first, in a single `io_uring_enter` that also submits every send and receive queued in that pass. The
  requests that arrive meanwhile share one wake-up, and their queries share a Postgres round trip. An idle
  thread blocks until the next completion, so the first request after a quiet spell is read at once; the cost is
  up to 2 ms of added latency on a busy thread.
- **Postgres**: exactly the SPEC's statements, one per request, prepared once per connection under short names.
  A request appends Bind/Execute to its connection's buffer; after the pass each connection with queued
  requests gets one send. Replies are walked in place in the receive buffer, and rows are turned into JSON
  straight from the DataRow bytes (ids and counts as binary integers, timestamps as binary microseconds
  formatted as UTC with milliseconds truncated).
  - **Reads share a Sync**: the reads a pass queues on one connection end with a single Sync, so Postgres runs
    them as one pack and flushes its replies once (it flushes at every Sync). Each read is answered at its own
    CommandComplete. Compared with a Sync per read, this cut Postgres segments per request by two thirds and
    Postgres CPU per request by 24% (see Measurements). If a read in a pack fails, Postgres skips the pack's later
    reads up to the Sync; those answer 500 (reads only fail when Postgres is in trouble).
  - **Writes get their own Sync**, so each is its own transaction: an error (a like on a missing post is SQLSTATE
    23503 -> 404) affects only that request, and the response goes out only after CommandComplete +
    ReadyForQuery, so it is committed.
  - **Routing**: reads and writes use separate connections, so a read never waits behind a commit. A read goes
    to the first live read connection with fewer than 64 requests in flight (so a pass's reads fill few
    packs), or the least loaded one when all are that full. A write goes to the least loaded write connection,
    so commits run side by side.
- **Reconnect**: when a Postgres connection ends (a closed socket, or a FATAL error such as an administrator
  terminating the backend), its queued requests answer 500 (a write is never retried) and `/health` 503 with no
  live connection. TCP keepalive and `TCP_USER_TIMEOUT` turn a connection that goes silent into an error after
  60 s, so its requests fail rather than hang forever; a shorter stall that resolves still answers every request.
  The event loop reconnects dead connections one per pass. A reconnect blocks its thread (1 s connect and 10 s
  read timeouts), so after a failed one the thread waits a second before trying again and keeps serving its other
  connections (and `/health`, with 503) in between.
- **HTTP/1.1, JWT, JSON input** are the same code as `rust-tokio-unorig-username` (`src/http.rs`, `src/json.rs`):
  request line, `Content-Length`, `Authorization`, `Accept-Encoding`, `Connection`, `Expect: 100-continue`;
  keep-alive by default; pipelined requests are answered in order (the next is parsed once the previous
  response is sent). HS256 checked on every request, `exp` required. Responses of 1 KB or more (the feed) are
  gzipped with libdeflate level 1 when asked.

## Measurements

**Challenge rig** (scored by the maintainer at `57ba6c5`, before reads shared a Sync): 1,000,000 (max) on 16
cores, 1,220,000 on 8, 1,170,000 on 4. Both the 8- and 4-core limits were latency at 36-64 % app CPU, with
Postgres and the load generators well under their limits.

**x86, before reads shared a Sync**: Xeon 8275CL, server limited to 4 cores, `box/setup-app-box.sh`, mload
`-gzip -think-scale 2.5` from separate load generators, Postgres on its own box. 1,000,000 users: pass, p95 9.3 ms,
p99 12.3 ms, 0 errors, about 21 us of server CPU per request, RSS 36 MB. 2,000,000 users: pass, 0 errors, 66 % of
the 4 cores, RSS 60 MB.

**Local A/B, per request.** Native arm64, 12 vCPU VM, kernel 6.8.0-117 with `box/setup-app-box.sh`'s sysctls.
Postgres 18 in its own container with `box/postgresql.conf`, a fresh copy of the seed per run. The server was
limited to 4 CPUs, and `bench/mload` ran on the other 8 over loopback: 60,000 users, `-gzip -think-scale 0.18`,
about 35,100 req/s. Each figure is counted over the same 40 s of the hold (about 1.40M requests); two
interleaved rounds agreed within 1 %.

| | Sync per read (#30) | shared Sync, packs of 16 | **shared Sync, packs of 64** | `c-epoll-unorig-username` |
|---|---:|---:|---:|---:|
| TCP segments, clients + Postgres | 4.34 | 3.62 | **3.45** | 3.48 |
| of which Postgres | 1.32 | 0.60 | **0.44** | 0.47 |
| Postgres CPU | 44.2 us | 41.4 us | **33.6 us** | 34.5 us |
| server CPU | 14.9 us | 15.0 us | **13.7 us** | 12.6 us |

Packs of 64 with 64 Postgres connections instead of 128 measured the same as packs of 64 with 128.

**x86 over a real network** (AWS, not the benchmark boxes): c6i.4xlarge app box on kernel 6.8.0-117-generic
with `box/setup-app-box.sh`, the NIC cut to one queue like the benchmark VM, and the server in `box/challenge.service`
with `AllowedCPUs=0-3` (RPS and the NIC's interrupt on the same cores). Postgres 18 on its own c6i.4xlarge via
`box/setup-db-box.sh` and `box/reset-db.sh`. mload `-gzip -think-scale 2.5` from a separate box. At 1,200,000
users, segments per request at the app box: 7.24 (#30), 6.18 (this), 6.27 (`c-epoll-unorig-username`), with server
CPU the same for all three (19.5, 19.5 and 19.3 us per request). The single load generator could not hold 1.2M
users smoothly, so these runs say nothing about the latency limit.

**Failures, locally.** 2,000 keep-alive connections sent mixed requests for 60 s at about 137,000 req/s, and every
response was checked against its own request (post id, like post_id, the new post's body and author). Meanwhile
Postgres cancelled 9,575 running queries and terminated 300 of the server's backends. Result: 8.2M requests, 0
mismatched responses, 0.27 % answered 500, and the new posts and likes in the database exactly equal the 201s
returned. The same check from the x86 load generator (4.5M requests, 120 backends terminated): 0 mismatched.

Under load (35,000 req/s), also locally:

- Postgres restarted: about 1 s of 500s, then all 128 connections back and latency normal.
- Postgres paused for 3 s: 3 s of latency, then normal (the same as #30).
- Postgres unreachable for 25 s (packets dropped, no reset): requests wait, then all succeed, 0 errors (the same
  as #30; a 10 s keepalive timeout tried first turned this into 19 % errors, hence 60 s).
- Postgres unreachable for 75 s: requests answer 500 once the 60 s timeout fires, the server keeps serving, and
  it recovers when Postgres is back.

HTTP edge cases beyond `test/test.sh` pass: 50 pipelined requests mixing reads and writes answered in order, a
request sent one byte at a time, `Expect: 100-continue`, `Connection: close`, gzip equal to the plain body, 3,000
clients that disconnect mid-query, and a connection idle for 80 s. `test/run.sh`: 42/42.

## Judgment calls

- `exp` is required (rule 9 says it is checked on every request); a token without `exp` is rejected.
- Chunked request bodies and requests over 64 KB close the connection (bodies over 16 KB are out of scope).
- Unknown methods on known paths answer 404 (out of scope in the SPEC).
