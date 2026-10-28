# Java 25 with no framework: epoll through FFM and the Postgres protocol in Java

The feed API from `SPEC.md`. At a million users the work per second is small (~42,000 requests, ~5% writes). What is
big is the **million open keep-alive connections**, and on 8 and 4 cores, how little CPU each request costs. So
this server has no framework, no JDBC and no object per connection: it calls epoll directly through Java's Foreign
Function & Memory API, speaks the Postgres wire protocol itself, and allocates almost nothing per request.

## Stack

| Piece | Version |
|---|---|
| Java | OpenJDK 25.0.4.1 (Ubuntu 24.04 `openjdk-25-jdk-headless`), ParallelGC |
| Runtime dependencies | none: the JDK and this code (a 102 KB jar) |
| Event loop | epoll, `accept4`, `read`, `write` through the Foreign Function & Memory API (no `java.nio` on Linux) |
| Postgres driver | written here: protocol 3.0, SCRAM-SHA-256 / MD5 / cleartext, prepared statements, pipelining, binary results |
| gzip | libdeflate 1.19 (Ubuntu 24.04 `libdeflate0`) through FFM; the JDK's zlib if it isn't installed |
| JWT | HS256 with the JDK's `javax.crypto.Mac` |
| Build | Maven 3.9.16 via the checked-in wrapper (`./mvnw`); JUnit 5.12.2 for tests only |
| Postgres server | 18 (separate box) |

## Design

**Thread-per-core, nothing shared.** One worker thread per usable core (`THREADS`; the default follows the CPU
affinity mask, so 16/8/4 cores need no setting). Each worker has its own epoll instance, its own `SO_REUSEPORT`
listener on ports 80 and 81 (the kernel spreads new connections over them), and its own Postgres connections. No
locks or atomics on the request path, no cross-thread hand-off.

**A connection costs about 200 bytes of heap.** A connection is not an object: its state lives in each worker's
arrays, indexed by a dense slot number (an fd-to-slot map finds it). An idle connection owns no buffer: requests are
read into a per-worker 64 KB scratch buffer and handled in place; only the rare partial request is copied out, and
freed once used. At 300,000 open connections the live heap after a collection was 53–68 MB.

**epoll through FFM, not `java.nio`.** `java.nio` keeps a `SocketChannel` and a `SelectionKey` per connection and
adds syscalls of its own (`fcntl`, `getsockname`, deregistration). Here a connection is an `int` fd and each request
is one `read` and one `write` on a level-triggered epoll instance; `EPOLLOUT` is watched only while a write is
pending. `accept4` makes sockets non-blocking at accept and the backlog is 65535. The listener has `TCP_NODELAY`,
which Linux copies to accepted sockets (checked on the first one; otherwise it is set on each), and
`TCP_DEFER_ACCEPT`: a connection is accepted once its first request is in, so a new client costs one wake-up, not
two. Running out of file descriptors pauses accepting for 100 ms instead of spinning. The `epoll_event` layout differs between x86_64
(packed, 12 bytes) and arm64 (16 bytes); both are handled and tested. A `java.nio` backend remains for macOS
development (`IO=nio`).

**Fewer, fuller wake-ups.** When a pass of the event loop handled fewer than `BATCH_MIN` sockets (default 32), the
worker pauses `BATCH_MICROS` (default 2,000 µs) before the next `epoll_wait`, so the next one brings more events and
each write to Postgres carries more queries. Responses and queries are already sent when it pauses, so only reading
waits. It adds up to 2 ms of latency at light load in exchange for fewer syscalls per request under load
(java-epoll-stohirov measured kernel time per request halving at ~40k req/s on one core).

**Postgres without JDBC.** Each worker holds its share of 64 read and 64 write connections (`DB_READ_CONNS`,
`DB_WRITE_CONNS`; 16 cores: 4 + 4 per worker; 4 cores: 16 + 16). Writes get their own connections, so a `/feed`
never waits in a pipeline behind an `INSERT`'s commit. The five statements are SPEC.md's SQL verbatim, prepared once
per connection. A read is `Bind, Execute, Sync`, its own implicit transaction, pipelined on the connection with
the fewest in flight, up to `PG_PIPELINE` (default 64) each. Writes use **group commit**: writes that arrive in the same
loop pass share one transaction (`Bind, Execute` per write, one `Sync`: one commit, one WAL flush), and a write
connection has at most one transaction in flight, so writes that arrive while every one is waiting for its commit
pile up and go out together next (up to 256). At the benchmark's write rate (~8%) most transactions hold one or two
writes; it caps the number of commits when writes come in bursts. If any write in a group fails, a
like on a missing post (SQLSTATE 23503, a 404) say, the whole transaction rolls back, and each write is sent again
in a transaction of its own, so every request gets the answer it would have got alone. A request that finds no
room, or older requests waiting, waits in a FIFO in the worker, already authenticated and validated. A response is
sent only after the `ReadyForQuery` that ends its transaction, so every write is committed first (rule 8). Results come back in binary and are
decoded straight into the JSON response: `int8`, `int4`, UTF-8 `text`, and `timestamptz` microseconds formatted as
`YYYY-MM-DDTHH:MM:SS.mmmZ`. A dropped connection fails its requests with 500 (a write whose commit is unknown is never retried)
and reconnects in the background, 50 ms backing off to 1 s; so does one that doesn't finish starting up within 5 s or
gets no answer to a query for 30 s. Past 65,536 waiting queries per worker, a request gets 503 instead of waiting.

**Almost nothing allocated per request.** The HTTP parser works on offsets into the read buffer, JSON is written by
hand into a reusable buffer with the headers written in front of the body afterwards (no copy), and the driver
decodes rows in place (a test checks it allocates nothing). At ~12,600 requests/s the server ran two young
collections of 20–23 ms in five minutes.

**gzip for the feed only.** When the client accepts gzip (`Accept-Encoding` parsed with q-values) and the body is at
least `GZIP_MIN` bytes (default 512, in practice only `/feed`), it is compressed in one call by a libdeflate
compressor each worker makes at startup, level 1 (`GZIP_LEVEL`; 0 turns it off). The body is copied into the
compressor's native buffer and the result back into a second response buffer that then swaps with the first: a
few KB of copying, far cheaper than the compression, and no allocation. Single posts (~300 bytes) and the small
replies go out uncompressed.

**The rules, checked on every request.** Every token's HS256 signature, `alg` and `exp` (and `nbf` if present) are
verified each time; nothing is cached across requests; every request reads Postgres while it is served.

**HTTP/1.1** keep-alive with no idle timeout, `Connection: close` and HTTP/1.0 keep-alive, `Expect: 100-continue`,
`Content-Length` bodies up to 16 KB, pipelined requests answered in order (one in flight per connection, the rest
wait in its buffer), and a half-closed client still gets its answer. A connection whose last response isn't fully
written yet handles no new request, so a client that sends without reading only fills its own 64 KB input limit. Chunked request bodies get a 400 (nothing
sends them).

## Code layout

| Package | What it holds |
|---|---|
| `feed` | `Main` and `Config` (environment variables, validated at startup) |
| `feed.server` | `Server` (one thread per worker, shutdown) and `Worker` (one event loop) |
| `feed.http` | `Connections` (the client connection table: reading, framing, writing, keep-alive), `HttpParser`, `ResponseBuffer`, `Gzip` |
| `feed.api` | `FeedApi` (routing, auth, validation), `QueryResults` (query results to responses), `Json`, `NewPostBody` |
| `feed.auth` | `JwtVerifier` (HS256) |
| `feed.json` | `JsonReader`: validates JSON and picks top-level members without building a tree |
| `feed.db` | `Database` (the worker's Postgres pool, pipeline cap, group commit and wait queue), `DbConnection`, `Queries` (SPEC.md's SQL) |
| `feed.pg` | the Postgres wire protocol: `PgProtocol` (a socket-free state machine), SCRAM, row and error views |
| `feed.net` | `Io` with `EpollIo` (Linux, through FFM) and `NioIo` (portable), and `FdMap` |

`perf/` is the load-test harness behind the numbers below, not part of the server.

## Settings

All optional; the defaults are what should be benchmarked.

| Env | Default | |
|---|---|---|
| `THREADS` | usable cores | worker threads |
| `DB_READ_CONNS` / `DB_WRITE_CONNS` | 64 / 64 | Postgres connections in total, split over the workers; together at most 512 |
| `PG_PIPELINE` | 64 | requests in flight per Postgres connection |
| `GZIP_LEVEL` | 1 | libdeflate 1–12 (the JDK fallback caps at 9); 0 = off |
| `GZIP_MIN` | 512 | smallest body that is gzipped |
| `BATCH_MIN` / `BATCH_MICROS` | 32 / 2000 | a loop pass with fewer sockets than this pauses this long; 0 µs = off |
| `HEAP` | 4g | `-Xmx` |
| `IO` | epoll on Linux | `nio` for the portable backend |

## Running

```bash
sudo ./install.sh   # OpenJDK 25 and libdeflate0, if missing
./build.sh          # target/feed.jar
DATABASE_URL=postgres://user:pass@host:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 ./start.sh
```

## Tests

`test/run.sh` passes 42/42 on Ubuntu 24.04 x86_64 (epoll, libdeflate); it also did on macOS (java.nio) before the
latest changes. `./test-pg.sh` runs 225 unit and integration tests (the ones that need Postgres each get a fresh copy of
`feed_seed`): the driver against a real Postgres and a scripted backend (split messages, mid-pipeline errors,
reconnects, no allocation, group commit and its retries), the HTTP parser, JWT, JSON output, both gzip compressors,
the epoll contract (Linux, deferred accepts included), the server over real sockets (pipelining, byte-by-byte
requests, `Expect`, half-close, slow readers, Postgres dropping every connection) and the pipeline cap and group
commit against a fake Postgres.

## Measured

Measured before group commit, wake-up batching and deferred accepts were added: on one shared 8 vCPU / 16 GB Ubuntu 24.04 VPS holding the server (4 cores, `taskset`), Postgres 18 (2 cores) and
`mload` (2 cores) over loopback, at the scoring settings (`-gzip -think-scale 2.5`), with `perf/run.sh`:

| Users | Requests | Errors | p95 | p99 | Server CPU / request | Server RSS |
|---|---|---|---|---|---|---|
| 50,000 | 344,073 | 0 | 1.2 ms | 2.7 ms | ~58 µs | 194 MiB |
| 300,000 | 3,198,997 | 0 | 150 ms | 971 ms | ~52 µs | 459 MiB |

At 300,000 users the server used about two-thirds of one of its four cores; Postgres and the load generator, on two
cores each, were the busy ones. The 300k p95/p99 come from one ~5 s stall in which the load generator's CPU rose
and the server's fell (5 s windows otherwise ran p95 ~7 ms, p99 ~15 ms). Over loopback the server's CPU also pays
for some of the load generator's network work, so these are a smoke test and a rough budget, not the scoring box's
numbers. `perf/setup.sh` and `perf/run.sh` reproduce them; [DESIGN.md](DESIGN.md) has the reasoning behind each
choice.

## Credits

Two entries already on the leaderboard shaped this one:

- [`rust-tokio-cknutson12`](../rust-tokio-cknutson12): most of the overall design, and the layout of this README.
  Thread-per-core with `SO_REUSEPORT` listeners and nothing shared, no buffer per idle connection (a per-thread 64 KB
  scratch buffer), separate pipelined read and write Postgres connections (64 + 64, split over the threads, fewest
  in flight first), the reconnect backoff, libdeflate level 1 for bodies of 512 bytes or more, and the HTTP/1.1
  behaviour.
- [`java-epoll-stohirov`](../java-epoll-stohirov): group commit with the one-by-one retry of a failed group,
  wake-up batching (`BATCH_MIN`, `BATCH_MICROS`, same defaults), and `TCP_DEFER_ACCEPT` with `TCP_NODELAY`
  inherited from the listener.

The code here is written from scratch; the ideas are theirs.
