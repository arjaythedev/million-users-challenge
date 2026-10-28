# Matt's cursed C++ io_uring

**Front: direct.** The server listens on `HOST:PORT` and `EXTRA_PORT` itself and holds every user's keep-alive connection.

| Piece | Version |
|---|---|
| Language | C++20, g++ from Ubuntu 24.04 (`build-essential`) |
| I/O | raw io_uring via liburing (Ubuntu 24.04 `liburing-dev`) |
| HTTP parsing | picohttpparser, pinned commit, SHA-256 checked in `build.sh` |
| JSON input | yyjson 0.10.0, SHA-256 checked in `build.sh` |
| Postgres driver | own Postgres wire-protocol client (pipelined extended protocol, binary results, prepared statements) driven by the worker's io_uring; libpq only to connect and authenticate |
| JWT | HS256, hand-written on OpenSSL SHA-256 (HMAC states precomputed), signature and `exp` checked on every request |
| Compression | Responses go out uncompressed while the link has room. When the server's own TCP counters show congestion (retransmits above 1%, local drops, RTT far above baseline, unsent socket backlog) and a cost model fed by timing both engines on 1 body in 256 says the CPU fits in the idle cores, it steps up to gzip with ISA-L igzip level 0 (Ubuntu `libisal-dev`, AVX2/AVX-512 picked at run time; libdeflate level 1 if ISA-L is not installed), and if congestion persists to libdeflate level 1 (about 15% smaller, about 2x the CPU per body). It steps back one level at a time when calm. `GZIP_MODE=none`, `light` or `full` pins a mode; `auto` is the default |
| Allocator | mimalloc 2.1.7, SHA-256 checked in `build.sh` |

Conformance: 42/42 on `test/run.sh`.

## Design

- One worker thread per CPU the process may run on (`sched_getaffinity`), so the same binary uses 16, 8 or 4 cores
  without configuration. Each worker owns an io_uring (`SINGLE_ISSUER | DEFER_TASKRUN`), its own `SO_REUSEPORT`
  listeners for `PORT` and `EXTRA_PORT`, its connection slab and its Postgres connections. Nothing on the request path
  is shared between workers.
- Multishot accept and multishot recv with a provided buffer ring: an idle keep-alive connection holds no receive
  buffer, only its small slab entry.
- **Batching window.** After a loop pass that found fewer than 32 completions, a worker waits for 8 of them (or 200 us) instead of
  waking for the first, so one wake-up and one syscall serve several requests; a busy worker never waits. Measured against a
  replay Postgres at 42k req/s on one core: 17.7 vs 21.1 us CPU per request; at 21k req/s: 22.7 vs 29.2 us. Median latency
  +0.4 to +0.9 ms. A window of 32 / 1 ms collapsed throughput and was rejected. `WAIT_N=1` turns it off, `WAIT_US` sets the cap.
- Each request is a C++20 coroutine that awaits one Postgres segment, so a worker never blocks.
- **One query per request.** All statements are prepared once per connection; a request is a Bind + Execute in a
  pipelined segment. Reads that queue while the connections are busy travel together in one segment (one Sync). They
  are not merged, de-duplicated or reused: every request gets its own result from Postgres, and nothing is cached.
- **Writes** (new post, like) use a dedicated connection per worker and group-commit naturally: writes queue while a
  segment is in flight and then share one Sync (one implicit transaction). Responses are released only after the
  commit. A failed group (for example a like on a missing post) is rolled back and replayed request by request; a
  connection lost after the Sync returns 500 and is never retried.
- Session settings on every Postgres connection: `jit = off`, `max_parallel_workers_per_gather = 0`,
  `plan_cache_mode = force_generic_plan`, `enable_seqscan = off` (every query has an index path).
- Postgres connections: `WORKERS x (DB_CONNS + 1)`, at most 512 (checked at startup; the read pool starts at 1 and grows to `DB_CONNS_MAX`, default 4).
- `created_at` is formatted straight from the binary timestamptz, truncated to milliseconds, UTC.
- **Buffer-ring workaround for Ubuntu 24.04 kernels 6.8.0-1xx.** Those kernels carry a backport whose reserved-field
  check in `IORING_REGISTER_PBUF_RING` is inverted, so every registration with zeroed reserved fields fails with
  EINVAL, liburing's included. On EINVAL the worker registers the ring by hand with a non-zero reserved byte, which that
  kernel accepts. A correct kernel never reaches the retry.

Environment: `DATABASE_URL`, `JWT_SECRET` (default `million-users-challenge`), `HOST`, `PORT`, `EXTRA_PORT`; optional
`WORKERS` (default: CPUs available), `DB_CONNS` (read connections per worker at start, default 1), `READ_GROUP_MAX` (most reads
in one pipelined segment, default 64), `GZIP_MIN` (smallest response that is compressed, default 512; 0 turns gzip off).
