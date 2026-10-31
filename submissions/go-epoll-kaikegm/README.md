# go-epoll-kaikegm: Native Linux Epoll + zerojwt + pgxpool Batch

A high-performance Go submission engineered specifically for **The Million Users Challenge** (`C1000K`).

Instead of the standard `net/http` model (1 goroutine and 2 buffered I/O streams per TCP socket, which consumes ~12 GB of user heap and overwhelms Go's GC mark phase at 1M connections), `go-epoll-kaikegm` implements an event-driven multi-reactor architecture directly on top of Linux `epoll(7)`.

| Piece | Version |
|---|---|
| Front: | direct (serves ports 80 & 81 simultaneously, no Nginx) |
| Go | 1.27.3 (`/usr/local/go`, official tarball with sha256 check), `CGO_ENABLED=0` |
| Reactor | Direct Linux Epoll (`golang.org/x/sys/unix`), Edge-Triggered (`EPOLLET`) |
| DB driver | `github.com/jackc/pgx/v5` (`pgxpool`), pipelined `pgx.Batch` (up to 32 queries/batch) |
| JWT | `github.com/KaikeGM/zerojwt/v2` v2.3.2 (Zero-alloc HS256 verification in ~12 ns) |
| Compression | `github.com/klauspost/compress/gzip` (Fast Level 1) |

## Key Optimizations for 1,000,000 Users

1. **Zero Goroutines for Idle Connections (C1000K):**
   - 1,000,000 idle keep-alive connections do **not** spawn goroutines or retain read/write heap buffers.
   - Sockets remain parked inside the Linux kernel epoll interest list.
   - Application user-space RAM for 1M idle connections is negligible (~16 MB), leaving over 20 GB of memory for the kernel's TCP socket slab.

2. **Lock-Free Epoll Reactor with `SO_REUSEPORT` & `TCP_DEFER_ACCEPT`:**
   - Spawns `GOMAXPROCS` worker threads, each bound to an OS thread via `runtime.LockOSThread()`.
   - Each worker creates its own `epoll` instance and binds listeners to both `PORT` and `EXTRA_PORT` using `SO_REUSEPORT` and `SO_REUSEADDR`.
   - Lock-free `hasPartial` atomic check ensures steady-state requests bypass any global mutex lock.
   - `TCP_DEFER_ACCEPT` prevents epoll wakeups until the client's first HTTP request payload actually arrives.
   - State cleanup (`clearConn`) runs strictly before `unix.Close(fd)` to prevent FD recycling race conditions.

3. **Pipelined Batching (`pgx.Batch`) & Late Materialization:**
   - DB workers drain up to 32 queued requests from the channel and pipeline them in a single `pgx.Batch` round-trip.
   - `/feed` uses CTE late materialization (`WITH page AS (...)`) to fetch top 20 posts before joining users and likes, preventing table scan regression under concurrent inserts.
   - DB connection configured with `plan_cache_mode = force_generic_plan`, `jit = off`, `max_parallel_workers_per_gather = 0`, and `enable_seqscan = off`.

4. **Zero-Allocation HTTP Parsing & Fast Body Extraction:**
   - Custom zero-alloc HTTP/1.1 byte-level parser using `bytes.Equal` method detection and `bytes.EqualFold` header matching.
   - Direct `parseContentLength` and `parseID` without string conversions.
   - Fast-path `fastParseBody` scans `{"body":"..."}` with zero heap allocations, falling back to JSON unmarshal for escaped or invalid bodies.
   - Direct byte buffer formatting for JSON responses (no reflection, zero `encoding/json` serialization overhead on hot paths).
   - `zerojwt/v2` validates token signatures and claims without heap allocations.

5. **Strict Read/Write Connection Separation & Batch Coalescing:**
   - **Isolated Read & Write Pools**: 75% of connection budget allocated to reads, 25% to writes.
   - Read queries (`/feed`, `/posts/:id`, `/health`) are processed by dedicated read workers over `readJobs` (65k capacity). A read batch **never** contains an `INSERT` and never stalls waiting for a WAL flush.
   - Write queries (`POST /posts`, `POST /posts/:id/like`) are processed by dedicated write workers over `writeJobs` (16k capacity), naturally group-committing in pipelined batches.
   - Micro-linger (500 µs on sub-threshold batches) eliminates lock contention and maximizes throughput.
   - Worker goroutines reuse preallocated scratch buffers for JSON formatting, HTTP headers, and gzip compression.
   - `sendAll` enforces a 100 ms deadline to ensure slow network clients cannot starve the database worker pool.
