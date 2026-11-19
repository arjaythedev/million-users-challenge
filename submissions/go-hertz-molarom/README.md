# go-hertz: Hertz on netpoll, parked keep-alive connections, pipelined pgx with group commit

|                |                                                                                                                                  |
| -------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| Language       | Go 1.27.3 (official tarball, `install.sh`)                                                                                       |
| Framework / IO | [Hertz](https://github.com/cloudwego/hertz) v0.10.6 on [netpoll](https://github.com/cloudwego/netpoll) v0.7.5 (epoll event loop) |
| Driver         | [pgx](https://github.com/jackc/pgx) v5.11.0: prepared statements, pipeline mode, group commit for writes                         |
| Front          | direct (ports `PORT` and `EXTRA_PORT`, one process, no Nginx)                                                                    |
| Other modules  | golang-jwt/jwt v5.3.1 (HS256), klauspost/compress v1.20.3 (gzip), `encoding/json/v2` (`jsontext`)                                |

## Running

```bash
sudo bash install.sh   # Go 1.27.3 into /usr/local/go (idempotent)
bash build.sh          # bin/feed
DATABASE_URL=postgres://feed:pw@db:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

Optional knobs (the defaults are what gets benchmarked): `DB_POOL` (128), `READ_WORKERS` (16), `WRITE_WORKERS` (8),
`MAX_BATCH` (512), `GROUP_COMMIT` (true), `IDLE_TIMEOUT` (120s, `0` disables it), `KEEP_ALIVE_TIMEOUT` (90s, TCP
keepalive), `READ_TIMEOUT` (30s, mid-request), `DEBUG_PORT` (off; pprof + expvar on 127.0.0.1 only).

## The goal

An ordinary Go server: a maintained framework, a normal driver, handlers that look like handlers,
and data access behind a `DBTX` interface (`Exec` / `Query` / `QueryRow`, the same shape as `pgx`). No custom
event loop, no hand-written HTTP parser or Postgres wire client. Everything below is tuning inside those limits,
with one exception: [parking idle connections](#parking-idle-connections).

## Design

- **HTTP: Hertz on netpoll.** netpoll is an epoll event loop, so reads and writes go through its buffers instead of
  a blocking `net.Conn` per connection. Compared with `net/http` on the same handlers, CPU per request was about
  28% lower and allocations about 55% lower. Both ports are served by two Hertz servers in one process that
  share the handlers and the database pipeline.
- **JSON output** goes through `encoding/json/v2`'s `jsontext.Encoder` with hand-written `MarshalJSONTo` methods,
  so the SPEC's key order is fixed and there is no reflection. Encoders are pooled. `created_at` is formatted on
  the stack as UTC, truncated to milliseconds.
- **gzip** only for `/feed` (about 5 KB down to about 2 KB) when the request sends `Accept-Encoding: gzip`. It uses
  pooled klauspost `gzip.Writer`s at `BestSpeed`, streaming the encoder straight into the response body. The
  other responses are small enough that compressing them costs more than it saves.
- **JWT**: golang-jwt with `WithValidMethods(HS256)` and `WithExpirationRequired()`, verified on every request.

## Postgres: `pgpipe`

The handlers call `a.reads` / `a.writes`, both `DBTX`. Behind them are two _lanes_, each a fixed set of worker
goroutines with one pooled connection each, fed by a channel of operations:

- **Reads (16 workers)** drain whatever is queued (up to `MAX_BATCH`) and send it as one pipeline, with a sync
  point after each query. That is one round trip for the whole batch, and each query still succeeds or fails on its own.
- **Writes (8 workers), group commit.** A batch of INSERTs is sent with a _single_ sync point, so Postgres runs it
  as one implicit transaction: one commit, one WAL flush for the whole batch. Each handler is answered only after
  that commit (rule 8). If one statement fails (for example a like on a missing post, SQLSTATE 23503 → 404), only
  that request gets the error and the rest of the batch is re-run with per-statement sync points. Deadlocks and
  serialization failures are retried.
- Exactly the SPEC's SQL, one statement per request, prepared once per connection. 24 backends are in use,
  `DB_POOL` caps the pool at 128, and the limit is 512.
- **Result buffering.** A `pgx.Rows` is only valid while its connection is, and here the connection belongs to
  the worker, not the handler. So each result is copied into a `bufferedRows` (a `pgx.Rows` implementation)
  before it is handed back. Each worker owns a reusable scratch arena, and the result is copied out once into a
  single exact-size slab, with values for single-row results stored inline in the struct. This cut `/feed`'s
  buffering from 69.6 KB / 29 allocs to 17 KB / 11 allocs. It was 64% of all allocated bytes before; in the
  200k-user window, total allocation fell from 3.04 GB to 1.31 GB.

## Parking idle connections

Out of the box, Hertz on netpoll still keeps **one goroutine per keep-alive connection**, parked in a read with an
idle timer while it waits for the next request. At 225k connections on 4 cpus that was the limit: each connection
held an 8 KB stack and a `RequestContext`, every GC cycle had to scan all of them, and resetting the per-connection
timers swept a timer heap with one entry per connection. Latency gave out at about 40% CPU.

netpoll itself doesn't need that goroutine: it calls the handler when bytes arrive, and an idle connection is just
an fd in epoll. Hertz has a mode for this (`IdleTimeout == 0`: return to the network layer after each response),
but `Engine.Serve` always closes the connection when it returns. `transport.go` is a small wrapper around Hertz's
own netpoll transporter that makes that close a no-op and closes the connection only when serving failed (EOF, a
malformed request, `Connection: close`). Hertz still parses every request and runs every handler; it just doesn't
hold a goroutine in between.

Without a parked read there is no read timer either, so `sweeper.go` enforces `IDLE_TIMEOUT` instead:

- Each connection records the time its last request finished and whether one is in flight.
- One goroutine sweeps the records every `IDLE_TIMEOUT / 8` (1–10 s). It closes connections idle past the timeout,
  never one with a request in progress, and forgets ones that are already closed. One O(connections)
  scan every 10 s instead of a timer per connection touched on every request.
- `KEEP_ALIVE_TIMEOUT` turns on TCP keepalive, so the kernel also reaps peers that vanished without a FIN.
  `READ_TIMEOUT` still applies to a request that arrives in pieces.

At 250k connections, the sweeper costs about 0.7% of CPU.

## Memory per connection

At a million keep-alive connections, memory per idle connection decides the score:

- **Parked connections** hold no goroutine stack and no `RequestContext` (it goes back to Hertz's pool after each
  request). That's about 8 KB of stack and about 1.5 KB of heap per connection.
- **netpoll buffers**: `LinkBufferCap` and `BufferSize` are raised to 16 KB. Below 8 KB netpoll keeps an idle
  connection's buffer forever; above it, the buffer is released. That saves about 8 KB per connection.
- **`WithMaxKeepBodySize(0)`** returns response-body buffers to the pool instead of keeping the last one on the
  connection (about 4.7 KB per connection before).
- **`GOMEMLIMIT` = 75% of RAM** (`start.sh`), so the GC goal stays clear of the kernel's socket memory.

Measured on the staircase (Linux VM, see below):

|                                     | goroutine per connection | parked   |
| ----------------------------------- | ------------------------ | -------- |
| Server RSS per conn                 | 24.6 KB                  | 7.0 KB   |
| Live heap per conn                  | 7.8 KB                   | 3.6 KB   |
| Goroutine stacks                    | 8.2 KB                   | ~0       |
| Goroutines per conn                 | 1                        | 0        |
| At 1M (+ ~4.3 KB kernel per socket) | 26.9 GiB                 | 10.5 GiB |

## Results (local estimate, not the official score)

Lima VM (Ubuntu 24.04 arm64): the server pinned to 4 cpus, Postgres 18 on 4 others, mload on 4 more, the
`setup-app-box.sh` sysctls, and `mload -gzip -think-scale 2.5`:

| Users (4 cpus) | req/s | p99, goroutine per conn | p99, parked (runs 1 / 2 / 3) | CPU util, parked |
| -------------- | ----- | ----------------------- | ---------------------------- | ---------------- |
| 100,000        | 4.6k  | 19 ms                   | 20 / 20 / 20 ms              | 12–14%           |
| 150,000        | 6.3k  | 21 ms                   | 24 / 22 / 20 ms              | 16–17%           |
| 200,000        | 8.5k  | 30 ms                   | 25 / 27 / 29 ms              | 21–23%           |
| 225,000        | 9.5k  | **1,105 ms (fail)**     | 460 / 214 / 596 ms           | 27–29%           |
| 250,000        | 10.6k | —                       | 348 / 981 / 40 ms            | 32–36%           |

Run 1 is before the sweeper; runs 2 and 3 have it, and run 3 is the submitted build. All three pass every step.
With connections parked, CPU per request stays flat from 100k to 250k (about 0.10–0.14 core-ms) instead of
growing with connections. The p99 at the top two steps jumps between runs without following the load (250k:
348, 981, then 40 ms; GC mark assist stays under 4%), so it looks like a GC cycle landing in the 30 s window,
not a capacity limit. 250k on 4 cpus is 62.5k users per core, the density 1M needs on 16 cores. The VM can't go
further: the load generator alone needs about 30 KB per connection. Memory caps any core count at about 2.8M.
The caveats: one VM stands in for three machines, traffic is over loopback rather than a NIC, and the cores are
Apple's, not Xeon. `test/run.sh`: 42/42.

The largest CPU costs at 250k are now the `read`/`write` syscalls (about 27%), gzip and JSON: request work,
not connection count. GC is about 10%.

## Also tried and dropped

- `sync.Pool`-ing `bufferedRows`: handing a pooled buffer to a handler made its lifetime hard to guarantee.
  The per-worker arena gets most of the win without that.
- Smaller netpoll buffers (2–4 KB): the gzipped feed plus headers doesn't fit, so buffers grow back on the first
  `/feed` and responses are flushed in more pieces.
- More read workers on the 4-cpu VM (8 vs 32): no difference there, because its Postgres has only 4 cpus. The default
  is 16 so that reads aren't capped at 8 backends on the real 16-core database box.
