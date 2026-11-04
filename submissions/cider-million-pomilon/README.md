# cider-million-pomilon

Feed API submission implemented in **Cider** ([github.com/Pomilon/Cider](https://github.com/Pomilon/Cider)), an ahead-of-time compiled, freestanding systems programming language.

- **Language:** Cider v4 (native x86-64 machine code generator)
- **Runtime:** Multi-worker shared-nothing architecture (`SO_REUSEPORT`, one worker per available CPU)
- **Direct or Nginx:** Direct on `PORT` and `EXTRA_PORT` (`80` and `81`, HTTP/1.1 keep-alive)
- **Database:** PostgreSQL 18 via `libpq` in non-blocking pipeline mode with prepared binary statements
- **Compression:** `libdeflate` gzip compression for responses $\ge 1\text{ KB}$ (`/feed`)
- **Purity:** 100% pure freestanding Cider application code in `src/server.cid`, with zero C application wrappers. Dynamically links system `libpq`, `libdeflate`, `libcrypto`, and `libc`.

## Design

- **Workers:** Spawns one worker process per CPU available to the process (counted with `sched_getaffinity`; workers are not pinned and are placed by the kernel scheduler).
- **Listeners:** Every worker binds its own listener with `SO_REUSEPORT` on both `PORT` and `EXTRA_PORT`. The Linux kernel distributes connections across workers via BPF hashing.
- **Postgres Pipeline:** Each worker maintains persistent non-blocking PostgreSQL connections in `libpq` pipeline mode (`PQenterPipelineMode`). Queries are dispatched with binary parameters (`PQsendQueryPrepared`), and results are streamed back asynchronously through the worker's event loop. Connect and prepare (`PQconnectStartParams`/`PQconnectPoll`/`PQsendPrepare`) run through the same epoll loop with bounded backoff, so DB outages never block request handling. Any pipeline desynchronization closes and reconnects the connection instead of matching stale results to later requests.
- **Bounded State:** Each worker's fd table is capped at 2,097,152 entries (enough for the 750k-per-worker worst case on four cores) instead of reserving the full 8M `RLIMIT_NOFILE`. Incomplete-request buffers are capped at 64 KiB per connection instead of growing without a fixed ceiling.
- **Idle Reaping:** A per-worker timer wheel closes connections idle for 120 s (above the 75 s keep-alive requirement) without scanning all connections on every event.
- **Recycled HTTP Buffers:** Requests are parsed in-place, and response headers and bodies use recycled per-worker buffer pools; slow/partial write paths may still allocate.
- **Strict Validation:** RFC-compliant JSON parsing, full Unicode/control character escaping, and constant-time HMAC-SHA256 JWT signature and expiration verification.

## Build and Run

```bash
# 1. Install toolchain & dependencies (Ubuntu 24.04, root)
sudo bash install.sh

# 2. Build server binary
bash build.sh

# 3. Run test suite
bash test/run.sh submissions/cider-million-pomilon

# 4. Start server in foreground
DATABASE_URL=postgres://USER:PASSWORD@HOST:PORT/feed_test JWT_SECRET=million-users-challenge HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```
