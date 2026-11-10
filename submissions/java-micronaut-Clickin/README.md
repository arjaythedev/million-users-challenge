# Micronaut + PostgreSQL

Micronaut HTTP server (Netty) with the Vert.x asynchronous PostgreSQL client for the [Million Users Challenge](../../SPEC.md).

## Stack

| Piece | Version |
|---|---|
| Java | OpenJDK 26.0.2.1 (SDKMAN `26.0.2.1+1-tem`) |
| Framework | Micronaut platform 5.2.2 (`micronaut-http-server-netty` 5.2.15) |
| HTTP / Transport | Netty 4.2.19.Final (native `epoll` on Linux, NIO fallback) |
| Postgres client | `io.vertx:vertx-pg-client` 5.2.1; platform-managed `vertx-sql-client` 5.2.0 |
| JSON | Micronaut Serde + `tools.jackson.core:jackson-core` 3.2.3 |
| Build tool | Maven 3.9.16 (`./mvnw`, `maven-shade-plugin` 3.6.1) |

## Run

```bash
sudo bash install.sh   # installs Temurin JDK 26.0.2.1+1 via SDKMAN in /opt/sdkman
bash build.sh          # ./mvnw -q -DskipTests clean package -> target/feed.jar
DATABASE_URL=postgres://feed:...@db:5432/feed JWT_SECRET=million-users-challenge HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

For local experiments, these variables override defaults. The challenge sets none of them.

| Variable | Default | Purpose |
|---|---|---|
| `MU_EVENT_LOOPS` | `nproc` | Netty event loops; each also runs its own Postgres pool |
| `MU_DB_POOL` | `64` | Total database connections, split evenly across the event loops (at most 512) |
| `MU_DB_PIPELINE` | `256` | Maximum pipelined requests per database connection |
| `MU_GZIP_LEVEL` | `1` at 8 or more CPUs; otherwise `0` | Gzip level; `0` disables compression |
| `MU_GZIP_MODE` | `loop` | Reuse compressors per Netty loop, or use `pool` |

Run `bash src/test/db-smoke.sh http://localhost:3000` against a server connected to the seeded database for a read-only smoke check.

## System Architecture

- **Single JVM process & Dual Listeners**: Runs in a single JVM listening on both `HOST:PORT` and `HOST:EXTRA_PORT` over HTTP/1.1 keep-alive (`idle-timeout: 120s`, `max-request-size: 16KB`).
- **Netty & Threading Model**: Uses 1 parent acceptor loop and `nproc` worker event loops with Linux native `epoll` (`nio` fallback). `micronaut.server.thread-selection: MANUAL` runs every route on its connection's event loop. There are no separate database threads: for each Micronaut event loop, a Vert.x context is built on that same Netty loop (`VertxInternal.contextBuilder().withEventLoop(...)`), and that loop gets its own Postgres pool. A request's query, its result and its response all run on the connection's own thread, with no handoff to another loop and back.
- **Database & SQL Execution**: Each event loop has a `PgBuilder.client()` pool of `ceil(64 / loops)` connections (64 in total by default) and pipelines up to 256 requests per connection. A Netty `FlushConsolidationHandler` at the head of each Postgres connection defers the client's per-query flush to the end of the loop pass, so the queries one loop issues while handling HTTP reads reach Postgres in one write. Connections open on demand, prepared statements are cached per connection, and query results use PostgreSQL's binary format. The TCP connection timeout and health-check deadline are 5 seconds, so a stalled database does not prevent a health response. It runs the reference SQL from `SPEC.md`, one query per request. Foreign-key violations (`SQLSTATE 23503`) on likes map to `404 post not found`, and `ON CONFLICT DO NOTHING` update counts distinguish `201` from `200`. The application waits for each query to finish before responding.
- **Why Pipeline**: Pipelining many requests over a few connections combines queries and responses into fewer packets and ACKs than one in-flight query per connection.
- **Stateless Auth & Compliance**: No response or query caching. Every authenticated request verifies the `HS256` signature and `exp` claim from scratch using thread-local `Mac` (`HmacSHA256`) instances.

## Notable Optimizations & Quirks

| Feature / Setting | Value | Why |
|---|---|---|
| Direct JSON Streaming (`DirectBodyWriter`) | Custom `ResponseBodyWriter` with `JsonRecyclerPools.newBoundedPool(64)` | Serializes `GET /feed` and `GET /posts/:id` responses without building intermediate JSON strings. |
| Event-loop-local Postgres pools | One pool per `((NettyHttpRequest<?>) request).getChannelHandlerContext().channel().eventLoop()` | The pool's connections live on the request's own loop, so results complete there and the response is written without a cross-thread dispatch. The previous layout (one shared pool on `nproc / 4` separate Vert.x loops, `nproc / 2` HTTP loops) hit its latency wall in the official runs at about half the box's CPU on 4, 8 and 16 cores. |
| Idle Timer Pruning | `read-idle-timeout: 0s`, `write-idle-timeout: 0s`, `idle-timeout: 120s` | Micronaut's HTTP/1.1 handler only closes idle connections on `ALL_IDLE`. Disabling the unused reader and writer idle timers cuts Netty `ScheduledFutureTask` allocations per connection from 3 to 1 (removing 2,000,000 scheduled timer tasks at 1M connections). |
| Response Compression | Off below 8 vCPUs; gzip level 1 at 8 or more | JSON bodies of at least 1,024 bytes, normally only `GET /feed`, use gzip when the client accepts it. Micronaut's built-in compressor is disabled because it creates an encoder for every response. `MU_GZIP_MODE=loop` (default) reuses a compressor per Netty thread and falls back to the pool on other threads, while `MU_GZIP_MODE=pool` always uses the pool. |
| JVM Memory & GC Flags | `-XX:+UseG1GC -XX:+UseCompactObjectHeaders -XX:InitialRAMPercentage=62.5 -XX:MaxRAMPercentage=62.5 -XX:MaxHeapFreeRatio=100` | Sets the initial and maximum G1 heap to 62.5% of the JVM-recognized RAM limit, giving allocations room and reducing GC pressure. This does not pre-touch 20 GiB of physical memory or reserve exactly 12 GiB for other uses. Compact object headers reduce live heap per idle connection, and JDK 26 includes the G1 write-barrier improvements in JEP 522. |
