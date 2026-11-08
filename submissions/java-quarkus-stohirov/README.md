# java-quarkus-stohirov: Feed API on Quarkus + the Vert.x reactive Postgres client

A production-style Quarkus service (JVM mode) built for ~1M keep-alive connections: Vert.x Web routes on Netty's
native epoll transport, every request handled on its event loop without blocking, and Postgres reached through the
Vert.x reactive client with pipelining. It passes the challenge test suite (`passed=42 failed=0`) on `PORT` and on
`EXTRA_PORT`.

## Stack (exact versions)

| Piece | Version |
|---|---|
| Java | OpenJDK 21 (Ubuntu 24.04 `openjdk-21-jdk-headless`; built and tested locally on 21.0.12.1) |
| Quarkus | 3.40.1 (LTS), `quarkus-vertx-http` + `quarkus-reactive-pg-client` |
| Vert.x (core, web, pg-client) | 4.5.34 (managed by the Quarkus BOM) |
| Netty | 4.1.138.Final, `netty-transport-native-epoll` (`linux-x86_64`); `kqueue` (`osx-aarch_64`) for local runs |
| JSON | jackson-core 2.21.7 streaming generator (no databind) |
| Postgres driver | Vert.x reactive PG client 4.5.34 |
| Postgres server | 18 (on another box, `DATABASE_URL`) |
| JWT | HS256 verified with the JDK's `javax.crypto.Mac` |
| Maven | 3.9.16 via the checked-in wrapper (`./mvnw`) |

## Design

- **One JVM, one event loop per usable core.** `start.sh` sets `quarkus.vertx.event-loops-pool-size` and
  `quarkus.http.io-threads` to `nproc` (which reads the systemd CPU affinity mask), instead of Quarkus's default of
  2 x cores: the kernel's network processing needs the rest. `IO_THREADS` overrides it.
- **No worker threads, no blocking.** Routes are registered straight on Quarkus's Vert.x `Router`
  (`@Observes Router` in `FeedRoutes`), with no RESTEasy or Mutiny layer. A handler checks auth and input, sends
  one query to the pool, and writes the response from the query's callback on the same event loop. An idle
  keep-alive connection is just a Netty channel, with no thread and no buffers attached.
- **Postgres: reactive pool with pipelining.** `DB_POOL` connections (default 64, the cap is 512) with
  `pipelining-limit=256`, so one connection carries many queries in flight instead of one round trip at a time.
  Statements are prepared once per connection (`cache-prepared-statements=true`). The SQL is the spec's,
  verbatim, one statement per request (`Sql.java`). `PoolWarmup` opens all `DB_POOL` connections at startup:
  the reactive pool otherwise connects lazily, so the start of the ramp would pay for up to 64 TCP + auth
  handshakes while requests queue behind them.
- **Two ports.** Quarkus serves `PORT`. `ExtraPortServer` deploys a second Vert.x HTTP server on `EXTRA_PORT`,
  one instance per event loop like Quarkus's own, with the same idle timeout, backlog, `SO_REUSEPORT` and
  compression settings, handing requests to the **same Router**. Both ports share one pool, one JWT verifier and
  the same handlers, and the full test suite passes on each.
- **Compression.** gzip level 1 (cheap CPU). Quarkus marks every response `Content-Encoding: identity` and only its
  annotated routes take that off, so `FeedRoutes.send` removes it for bodies of 256 bytes or more. The feed
  (5.3 KB) goes out as ~1.9 KB. Error bodies and short single posts stay plain, because gzip would make them
  bigger.
- **JWT on every request.** Split, base64url-decode, require `alg` = `HS256`, constant-time HMAC compare
  (`MessageDigest.isEqual`, one `Mac` per event-loop thread), then `exp`, then `sub`/`username`. Nothing is cached.
- **Errors** are preallocated `ApiError` exceptions without stack traces. A like on a missing post fails the
  foreign key (SQLSTATE 23503) and returns 404. Any other database error returns 500 and is logged.
- **`created_at`** is read as an `OffsetDateTime` and formatted by hand in UTC with the milliseconds truncated.
- **JSON** is written with jackson-core's streaming generator straight into the response `Buffer` (one copy).
  The first JFR profile showed ~46% of all allocation was the body being copied twice: into a `byte[]`, then
  into the `Buffer`.

## Non-default settings and why

| Setting | Value | Why |
|---|---|---|
| `quarkus.vertx.prefer-native-transport` | true | Netty epoll instead of NIO: less garbage and lower syscall overhead per connection. Confirmed in the startup log (`native transport true`). |
| `quarkus.http.idle-timeout` | 120s | Clients sit idle up to ~40 s; the spec asks for >= 75 s. Quarkus's default (30 min) would keep dead peers around. |
| `quarkus.http.accept-backlog` | 65535 | Matches the box's `somaxconn` for ramp-up bursts. |
| `quarkus.http.so-reuse-port` | true | Lets the event loops' listeners share the port. |
| `quarkus.http.enable-compression` / `compression-level` | true / 1 | gzip when `Accept-Encoding: gzip` is sent, at the cheapest level. |
| `quarkus.http.limits.max-body-size` | 16K | Request bodies are under 1 KB. |
| `quarkus.datasource.reactive.max-size` | `DB_POOL`, default 64 | With pipelining, 64 connections give Postgres parallelism across its 16 cores well within the 512 cap. All are opened at startup (`PoolWarmup`). |
| `quarkus.datasource.reactive.postgresql.pipelining-limit` | 256 | Queries in flight per connection. |
| `quarkus.datasource.reactive.cache-prepared-statements` | true | Prepare once per connection. |
| Event loops | `IO_THREADS`, default `nproc` | One per usable core (Quarkus's default is 2 x cores). |
| JVM `-XX:+UseG1GC` | G1 | Set explicitly: with 1 usable CPU the JVM's ergonomics would pick SerialGC. |
| JVM `-XX:MaxRAMPercentage` | `MAX_RAM_PCT`, default 50 | ~16 GB of heap. Measured live heap is ~4.5 KB per open connection (below), so ~4.5 GB at 1M, and half of RAM leaves room for direct buffers and the kernel's socket memory. |
| JVM `-XX:MaxDirectMemorySize` | `MAX_DIRECT`, default 6g | Caps Netty's pooled direct buffers so heap + direct stays under 32 GB. |

There is no per-request logging. Failures log at ERROR.

## Files

- `install.sh`: installs `openjdk-21-jdk-headless` if JDK 21 is missing (same as `examples/java`).
- `build.sh`: `./mvnw -B -q -DskipTests package` builds the fast-jar `target/quarkus-app/`.
- `start.sh`: runs `target/quarkus-app/quarkus-run.jar` in the foreground with the JVM flags above.
- `src/main/java/com/example/feed/`: `FeedRoutes` (routes and handlers), `ExtraPortServer` (the `EXTRA_PORT`
  listener), `PoolWarmup`, `Jwt`, `Json` (response writer and request-body validation), `Sql`, `ApiError`, `Ids`.

## Results so far (local only, not the benchmark box)

Measured on a MacBook (Apple silicon, 18 cores, kqueue transport) with Postgres 17, the server and the load
generator all on the same machine. These numbers are for orientation only.

- `bash test/run.sh submissions/java-quarkus-stohirov`: `passed=42 failed=0`. `test/test.sh` against
  `EXTRA_PORT` on a fresh database copy: `passed=42 failed=0`.
- Linux x86_64 (an `ubuntu:24.04` amd64 container, emulated): `install.sh` (OpenJDK 21.0.12.1 from apt),
  `build.sh`, then `test/run.sh`: `passed=42 failed=0`. The startup log shows `native transport true`
  (Netty epoll) and `Postgres pool warmed: 64/64 connections open`.
- `bench/mload` with 15,000 users over both ports (`-gzip -think-scale 2.5 -ramp 60s -hold 90s`):
  `RESULT users=15000 req/s=558 p95=4.6ms p99=9.2ms err=0.000% pass=true`. (macOS gives one client IP only
  ~16k ephemeral ports across all destination ports, so this is as many connections as a single local run
  can open.)
- Memory at that hold, after a full GC: live heap 82 MB vs 15 MB idle, i.e. **~4.5 KB per open connection**
  (a Netty channel, its 7-handler pipeline, the Vert.x connection, decoder, compressor and idle-timeout
  handler); process RSS 324 MB vs 166 MB idle. For comparison, the Spring/Tomcat example measured 9.9 KB of
  heap per connection with shrunken socket buffers.
- `ab -k`, gzip, three `ab` processes at once on one machine (the load generator is the limit here, not the
  server): `GET /feed` ~26k req/s + `GET /posts/:id` ~42k req/s + `POST /posts/:id/like` ~17k req/s, 0 non-2xx.

## Next steps

- Load test with `bench/mload` on the real boxes, then tune `DB_POOL`, `IO_THREADS` and the heap from there.
- Group commit: batch concurrent `INSERT`s for likes and posts into one statement or transaction (allowed by
  rule 8, as long as each response waits for its commit).
- Try Netty's io_uring transport.
