# rust-uring: thread per core, raw io_uring, hand-written Postgres wire client

| | |
|---|---|
| Language | Rust 1.94.0 (edition 2024), rustup in `/opt/twelve-rust` like the repo's Rust example |
| Framework / IO | none; `io-uring` 0.7.15 (the kernel interface only, no runtime), one ring per thread |
| Driver | none: about 370 lines in `src/pg.rs` speak the Postgres v3 protocol (named prepared statements, Bind/Execute/Sync, binary results), trust, cleartext or SCRAM-SHA-256 auth, no TLS |
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
  receive that draws from a per-thread ring of 1,024 provided 2 KB buffers, so an idle connection owns no
  receive buffer: the kernel picks a buffer only when bytes arrive, and the request is parsed in place and the
  buffer handed back at once.
- **Per-connection state is 16 bytes** (generation, state, flags, and a boxed buffer that exists only while a
  partial or pipelined request waits).
- **Limits**: the registered file table is 1,048,576 entries per thread (the kernel's maximum), so 4 threads
  hold up to about 4M connections; the slot field in a completion's tag is 24 bits.
- **One wait per batch**: after a pass that did work, the thread waits for 128 completions or 2 ms, whichever
  comes first, in a single `io_uring_enter` that also submits every send and receive queued in that pass. The
  requests that arrive meanwhile share one wake-up, and their queries share a Postgres round trip. An idle
  thread blocks until the next completion, so a lone request is answered at once.
- **Postgres**: exactly the SPEC's statements, one per request, prepared once per connection under short names.
  A request appends Bind/Execute/Sync to its connection's buffer; after the pass each connection with queued
  requests gets one send. Replies are walked in place in the receive buffer: each request ends at its
  ReadyForQuery, and its rows are turned into JSON straight from the DataRow bytes (ids and counts as binary
  integers, timestamps as binary microseconds formatted as UTC with milliseconds truncated). Sync per request
  keeps every request in its own implicit transaction, so an error (a like on a missing post is SQLSTATE
  23503 -> 404) affects only that request. Writes autocommit and the response goes out only after
  CommandComplete + ReadyForQuery, so it is committed. Reads and writes use separate connections
  (least requests in flight, ties to the lowest slot), so a read never waits behind a commit.
- **Reconnect**: when a Postgres connection ends, its requests in flight answer 500 (a write is never retried)
  and `/health` 503 with no live connection. The event loop retries a dead connection at most once a second.
- **HTTP/1.1, JWT, JSON input** are the same code as `rust-tokio-unorig-username` (`src/http.rs`, `src/json.rs`):
  request line, `Content-Length`, `Authorization`, `Accept-Encoding`, `Connection`, `Expect: 100-continue`;
  keep-alive by default; pipelined requests are answered in order (the next is parsed once the previous
  response is sent). HS256 checked on every request, `exp` required. Responses of 1 KB or more (the feed) are
  gzipped with libdeflate level 1 when asked.

## Measurements (local, not the benchmark boxes)

Native arm64 in a 12 vCPU VM on a macOS host (shared with other benchmarks, so latencies are noisy), Postgres 18
in its own container with `box/postgresql.conf` (shared_buffers 2 GB), full seed, fresh copy per run. Load: the
repo's `bench/mload` over loopback, `-gzip`, 90 s hold. Server pinned to 4 cores (`taskset -c 0-3`), load
generator on the other 8. Server CPU is user + sys of the server process over 70 s of the hold, per request.
Compared in the same lock-held sets against `rust-tokio-unorig-username` built the same way.

- `test/run.sh`: `passed=42 failed=0` in a clean container that ran `install.sh`.
- 60,000 users, `-think-scale 0.18` (~35,100 req/s, the per-core rate of a million users on 5 cores):
  **14.5 us of server CPU per request** (5.1 user + 9.3 sys; rust-tokio 21.9), p95 8 ms, Postgres CPU 46 us per
  request (rust-tokio 53: the read packing means fewer, fuller round trips).
- 180,000 users, `-think-scale 0.54` (same request rate): 14.8 us per request, **27 MB RSS** (rust-tokio 170 MB):
  50 bytes of user memory per added connection, on top of about 18 MB of fixed buffers. The kernel's socket and
  io_uring request memory per connection comes on top, as for any server.
- Not adopted: a 4 ms window instead of 2 ms (15.0 vs 15.7 us per request, p95 doubles).

## Judgment calls

- `exp` is required (rule 9 says it is checked on every request); a token without `exp` is rejected.
- Chunked request bodies and requests over 64 KB close the connection (bodies over 16 KB are out of scope).
- Unknown methods on known paths answer 404 (out of scope in the SPEC).
