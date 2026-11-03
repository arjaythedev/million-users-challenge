# Java + Vert.x + PostgreSQL

| | |
|---|---|
| Language | Java 26, Temurin 26.0.2.1+1, which `install.sh` installs through SDKMAN. The compiler targets Java 21 bytecode |
| Framework | Vert.x Core 5.2.1 `HttpServer` without Vert.x Web, on Netty 4.2.19.Final with native epoll |
| Postgres driver | Vert.x PostgreSQL client 5.2.1 with prepared statements and pipelining |
| JSON | A hand-written byte writer for responses. Jackson Core 3.2.3, streaming only, parses request bodies and JWTs |
| GC | Generational ZGC. The JVM sizes the heap, up to 75% of RAM |
| Build | Maven 3.9.16 wrapper and the Shade plugin 3.6.1, producing `target/feed.jar` |

## Running it

```bash
sudo bash install.sh
bash build.sh
DATABASE_URL=postgres://feed:password@db:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

Optional environment variables:

| Variable | Default | Effect |
|---|---|---|
| `MU_EVENT_LOOPS` | `nproc` | Number of event loops, one verticle each |
| `MU_DB_POOL` | `64` | Total Postgres connections, split over the loops, at most 512 |
| `MU_DB_PIPELINE` | `256` | Queries in flight per Postgres connection |
| `MU_GZIP_LEVEL` | `1` on 8 or more CPUs, else `0` | gzip level for the feed. `0` turns gzip off |
| `MU_HEAP` | unset, so up to 75% of RAM | Maximum Java heap size, passed as `-Xmx` |

## Design

**One verticle per event loop.** Each verticle listens on both ports and has its own pipelined `PgBuilder.client()`
with a pool event-loop size of 0. The same thread parses a request, runs its query, decodes the rows and writes the
response. Each verticle also keeps its own prepared queries, response buffer, HMAC and gzip state, so the loops share
nothing.

**Postgres.** The server runs the SPEC's SQL, one query per request. It prepares each query once per connection and
pipelines up to 256 queries on each of the 64 connections. The pg client flushes its socket after every query, so
by default each request costs one `write` to Postgres. A Netty `FlushConsolidationHandler` at the head of each
Postgres connection's pipeline defers those flushes until the event loop has handled all ready I/O. Queries from all
requests that arrived together then go out in one `write`.

**HTTP server.** Strict thread mode is on. The server never closes idle connections, which meets the SPEC's 75 s
minimum and removes Netty's per-connection idle timer. Compression, the h2c upgrade and WebSocket compression are off.
With them on, Vert.x creates an HTTP/2 initializer and two WebSocket deflate handshakers for every connection, about
120 MB at a million connections. A small `EpollTransport` subclass sets `TCP_DEFER_ACCEPT` on the listening sockets,
so the kernel hands over a connection only after its first request arrives. It also drops the `TCP_NODELAY` and
`SO_KEEPALIVE` options that Vert.x would set again on every accepted socket. Netty already turns on `TCP_NODELAY`
when it creates the channel, and keep-alive is off by default.

**Allocation per request.** The server writes response JSON into a per-loop byte array and copies it once into a
buffer of the exact size. It uses no JSON generator and creates no intermediate strings. It formats `created_at`
directly from the `OffsetDateTime` that the driver returns, writes numbers two digits at a time, and bulk-copies
strings, switching to character-by-character escaping only at the first character that needs it. Header names and
values are encoded once at startup, `Content-Length` values come from a table, the code reads rows by column index,
and 200 responses skip `setStatusCode`.

**JVM.** Each of a million connections keeps about 2.7 KB of live objects for the Netty channel, its pipeline and the
Vert.x connection, about 3 GB in total. All of these objects are created while users ramp in. ParallelGC and G1 paused
for 140 to 830 ms per young collection at the end of the ramp. ZGC does this work concurrently, so it is the collector
here.

The heap has no fixed size and is not pre-touched. ZGC grows it as needed, up to 75% of RAM. At a million connections on
a 32 GB machine it committed about 21 GB. `-XX:-ZUncommit` keeps ZGC from returning that memory to the OS, so the
kernel zero-fills each heap page only once.

On Java 25 and later, Netty 4.2 stops using `sun.misc.Unsafe` by default and falls back to `ByteBuffer` and
`MemorySegment` access. That path adds bounds and session checks to every buffer access and allocates a
`DirectByteBuffer` per buffer, which was 17% of all allocation in a profile. `--sun-misc-unsafe-memory-access=allow`
restores the `Unsafe` path.

**gzip on 8 or more CPUs.** The server compresses bodies of at least 1,024 bytes, which in practice means the feed,
at level 1 with a reused `Deflater`. On 4 cores gzip cost more CPU than it saved, so it is off below 8 CPUs.

## Measurements

I measured on Hetzner Cloud, not on the benchmark machines. The app ran on a CCX33 (AMD, 32 GB, virtio NIC with one
queue) with all but 4 CPUs taken offline. Postgres ran on a CPX42. Four load generators (CCX23, CAX31, CCX13, CAX21)
drove 330,000, 330,000, 170,000 and 170,000 users with `bench/mload -gzip -think-scale 2.5`. The ramp took 330 s,
about 3,000 new connections per second, and the hold lasted 5 minutes.

At 4 cores and 1,000,000 users the server passed in both runs:

| Run | Requests/s | p95 | p99 | Max | Errors |
|---|---|---|---|---|---|
| 1 | 32,062 | 41.9 ms | 207.7 ms | 897 ms | 0 |
| 2 | 32,063 | 37.5 ms | 148.1 ms | 804 ms | 0 |

CPU peaked at 95 to 97% at the end of the ramp. During the hold it was 62 to 63%: 17% user time and 45 to 46% kernel
time. Postgres stayed under 38% CPU and the load generators under 35%. GC logged no allocation stalls. I did not
measure 8 or 16 cores.

## Checks

```bash
./mvnw -q -o -DskipTests clean package
bash src/test/check.sh
RECOVERY_DATABASE_URL=postgres://feed:feedpw@localhost:15432/feed_test bash src/test/recovery-check.sh
```

`check.sh` tests the `start.sh` defaults and the HTTP behaviour, compares the JSON writer with Jackson byte for byte,
and on Linux checks that the server reports why epoll is unavailable. `recovery-check.sh` checks that `/health`
recovers after Postgres comes back.
