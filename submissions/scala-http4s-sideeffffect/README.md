# Scala 3 + Cats Effect + http4s Ember + skunk

| | |
|---|---|
| Language | Scala 3.10.0 on Temurin JDK 27 (`27+35`; official tarball, checksum-verified) |
| Build | Scala CLI 1.18.0 (official release binary, checksum-verified); build definition in `project.scala`; `start.sh` runs the app through it too |
| Effect system / streaming | Cats Effect 3.7.1, fs2 3.14.0 |
| HTTP server | http4s 0.23.38, Ember backend (`http4s-ember-server`), routes in `http4s-dsl` |
| Postgres driver | skunk 2.0.0-RC3 (the Typelevel Postgres driver on fs2 sockets; no JDBC), otel4s 1.1.0 no-op providers |
| JSON | circe 0.14.17 (derived encoders, jawn parser) |
| JWT | HS256 with the JDK's `Mac` (`Jwt.scala`) |
| GC / heap | Generational ZGC, 75% of RAM, pre-touched |

This is ordinary Typelevel code: one `HttpRoutes[IO]` with a case per endpoint, the SQL from [SPEC.md](../../SPEC.md)
verbatim as skunk statements, and circe encoders. There is no hand-written HTTP parser or event loop. One process
serves `PORT` and `EXTRA_PORT` with two Ember servers that share the app and the database sessions.

**[FINDINGS.md](FINDINGS.md)** has every decision with its measurements, and the ideas that were rejected.
**[RESEARCH.md](RESEARCH.md)** is the survey of the six examples and the other submissions it started from, with
memory per idle connection across all of them.

## Running it

```bash
sudo bash install.sh   # JDK 27 and Scala CLI into /opt
bash build.sh          # scala-cli compile, writes target/classpath.txt
DATABASE_URL=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

Optional knobs (env): `DB_READ_SESSIONS` (default 128), `DB_WRITE_SESSIONS` (default 32), `COMPUTE_THREADS`
(default: all cores), `MAX_CONNECTIONS` (per port, default 1,000,000), `HEAP_PERCENT` (default 75).

## Layout

The sources are in `src/`. `project.scala` holds the Scala version, the compiler options and the pinned dependencies.

| File | What |
|---|---|
| `Main.scala` | Config from env, the skunk sessions, one Ember server per port |
| `Api.scala` | The five routes, auth, validation, the error mapping, JSON (and gzip) responses |
| `Sql.scala` | The SPEC.md SQL as skunk `Query`/`Command` values |
| `SessionPool.scala` | Lends long-lived skunk sessions from a Cats Effect `Queue` |
| `Jwt.scala` | HS256 + `exp` verification, then the `sub`/`username` payload checks |
| `Gzip.scala` | One-shot gzip of a small, complete body with a per-thread `Deflater` |
| `ListenBacklog.scala` | Raises the listen backlog (see below) |
| `Model.scala` | Response types with circe encoders, the timestamp format, `ApiError` |

## Settings that are not the default, and why

| Setting | Value | Why |
|---|---|---|
| Ember `maxConnections` | 1,000,000 per port | The default is 1,024. Up to ~500,000 connections arrive on each port. |
| Ember `idleTimeout` | none (`Duration.Inf`) | Users idle up to ~40 s, and the rules ask for at least 75 s. A finite timeout races a timer against every read: more CPU per request and more heap per idle connection. |
| Ember `receiveBufferSize` | 8 KB (default 256 KB) | fs2 gives every socket an 8 KB read buffer. A larger read size grows that buffer for each connection. Requests are under 1 KB. |
| Listen backlog | 65,535 (kernel caps it at `somaxconn`, 65,535 on the box) | fs2 binds with Java's default of 50, and Ember has no setting. When the accept queue overflows during the ramp, each dropped SYN costs the client a 1-3 s retransmit. On Linux, a second `listen()` on a listening socket changes its backlog, so `ListenBacklog.scala` finds both sockets in `/proc/self/fd` and does that through the JDK's foreign function API. |
| Cats Effect compute threads | one per core | Postgres is on the other machine, so all cores serve HTTP. |
| CPU starvation checker | off | Its warnings only add noise under load. |
| Fiber tracing | off (`cats.effect.tracing.mode=none`) | It records a stack frame for every `IO` step, which costs CPU on every request. |
| Database sessions | 128 read + 32 write = 160 (limit: 512) | Skunk runs one statement at a time per session. A request that writes uses a write session, so a burst of reads can't queue the writes. |
| Session pool | own (`SessionPool.scala`) | skunk's `Session.pooled` runs `UNLISTEN *` and `RESET ALL` on every return: two extra round trips per request. This pool keeps a fixed set of sessions in a `Queue` and replaces one only when it is unhealthy or left inside a transaction. |
| Session settings | `plan_cache_mode = force_generic_plan`, `jit = off` | Session-level `SET`s (allowed by rule 2), sent in the startup packet. One generic plan per prepared statement from the first run, and no JIT for short index lookups. |
| Prepared statements | once per session | skunk caches each statement's prepare per session. That caches plans, not data (rule 7). |
| skunk telemetry | no-op tracer and meter, query capture off | otel4s instruments every statement. The no-op providers make that free. |
| gzip | level 1, bodies of 1 KB or more, when `Accept-Encoding: gzip` | The feed is 3.6 KB as JSON and 0.75 KB gzipped, which matters for the box's network at ~42,000 req/s. http4s's `GZip` middleware streams every body through fresh 32 KB buffers with chunked encoding. `Gzip.scala` compresses the complete body in one call with a reused per-thread `Deflater` and keeps `Content-Length`. Smaller bodies (one post, errors) go out as they are. |
| JVM GC | `-XX:+UseZGC` (generational; the only ZGC mode in JDK 27) | See below. |
| JVM heap | `-XX:MaxRAMPercentage=75 -XX:InitialRAMPercentage=75 -XX:+AlwaysPreTouch` (24 GB of 32 GB) | Postgres is on the other machine. The rest (8 GB) is for the kernel's socket memory (about 3 GB for a million TCP sockets), the JVM's own memory and the OS. Fixed and pre-touched: there is no swap, and a heap that grows later could fail. |
| JVM | `--enable-native-access=ALL-UNNAMED` | Allows the `listen()` call above without a warning. |

There is no request logging. Only unexpected errors (500) are printed.

## Postgres access

- Each request runs one statement from SPEC.md, in autocommit. `POST /posts` and `POST /posts/:id/like` reply after
  their insert has committed.
- Writes are not batched. At ~5% of 42,000 req/s that is about 2,000 inserts per second, and Postgres already
  shares one WAL flush among commits that arrive together.
- A like on a missing post fails the foreign key (SQLSTATE 23503), which maps to 404 `post not found`. Inserted or
  not comes from the command's row count (`INSERT 0 1` or `INSERT 0 0`).
- `created_at` is read as `timestamptz` and formatted in UTC with `SSS`, which truncates to milliseconds.

## Heap per idle connection (the design constraint)

At a million users the server holds a million open keep-alive connections, so the heap that one idle connection
costs decides how many users fit. Measured on JDK 27 with ZGC: 20,000 connections (2 addresses x 2 ports x 5,000),
each idle after one request, live heap from `jcmd GC.class_histogram` (it runs a full GC) before and after:

| After one request to | Live heap per idle connection |
|---|---|
| `/health` | 38.1 KB |
| `/feed` (gzipped) | 38.1 KB |

The response is not kept. What is kept (per connection, from the histogram):

| What | Per connection |
|---|---|
| `byte[]`: fs2's socket read buffer (8 KB, allocated when the socket is made, kept while it is open) | 8.6 KB |
| Cats Effect: 10 fibers, their stacks (`Object[]`) and suspended `IO` continuations | 13.8 KB |
| fs2: 21 scopes, 18 interrupt contexts, suspended `Pull` continuations | 10.0 KB |
| atomics, `Some`, `Chain`, http4s, the rest | 6.4 KB |

Ember runs each connection as an inner stream of `parJoin`, inside interruptible fs2 scopes. Each layer keeps its own
scope, fibers and continuations while the connection waits for its next request. Ember has no setting that changes
this, and fs2 always allocates the 8 KB read buffer. The settings above (no idle timeout, an 8 KB read size, no
tracing) are the ones that keep it from growing.

**What this means for the score.** 38 KB x 1,000,000 connections is about 38 GB, more than the whole box. A 24 GB
heap holds at most about 630,000 idle connections before any request garbage, so this server should top out at
roughly 500,000-600,000 users, from heap, not CPU. More heap does not fix it: the kernel needs about 3 GB for the
sockets. Reaching a million with http4s means less per-connection state than Ember keeps (for example one fiber per
connection that waits for the socket to be readable before it takes a buffer). That is a different server, not a
setting.

## Garbage collector

- **Generational ZGC.** The live set is large (gigabytes of connection state) and changes all the time as users come
  and go. ZGC marks and moves it concurrently, so pauses stay under a millisecond whatever the heap size.
- Our earlier [GC showcase](https://github.com/sideeffffect/multicore-server-challenge/blob/gc-showcase/experiments/gc-showcase/README.md)
  (same stack, 100,000 idle-heavy keep-alive connections, ~3.5 GB of connection state) measured this: Parallel GC
  had full collections of up to 1.7 s and a p99.9 of 2.1 s. G1 had pauses up to 155 ms. ZGC had a p99 of 28 ms and
  a p99.9 of 110 ms, at ~40% more app CPU than Parallel.
- Here the live set is ~7x larger, and the score is on p95 and p99. A stop-the-world collector would stop every one
  of the open connections for seconds. ZGC's extra CPU is the price for that.

## Local numbers

On a laptop (16 logical CPUs shared with Postgres 18 in Docker and the load generator), `bench/mload` with `-gzip
-think-scale 2.5`, both ports, a 30 s ramp, the server in a 12 GB cgroup (9 GB heap):

| Users | Hold | req/s | p95 | p99 | Errors |
|---|---|---|---|---|---|
| 20,000 | 60 s | 840 | 1.2 ms | 1.9 ms | 0 |
| 40,000 | 90 s | 1,630 | 1.2 ms | 2.0 ms | 0 |

The test suite passes 42 of 42 (`test/run.sh`). A laptop runs out of source ports long before the server runs out of
anything, so these numbers only show that it works, not where its limit is.

## License

MIT, under the repo's [license](../../LICENSE).
