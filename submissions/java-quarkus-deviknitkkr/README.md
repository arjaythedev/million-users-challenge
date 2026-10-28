# java-quarkus-deviknitkkr

Quarkus feed API using standard reactive REST controllers and the reactive PostgreSQL driver.

| | Version |
|---|---|
| Runtime | OpenJDK 25 (bytecode targets Java 21; JDK 21 also works) |
| Framework | Quarkus 3.40.1 (Quarkus REST + Vert.x 4.5) |
| Database driver | Vert.x reactive PostgreSQL client 4.5.34 (via `quarkus-reactive-pg-client`) |
| JSON | Jackson + Blackbird |
| JWT | Auth0 java-jwt 4.6.2 |
| Network transport | Netty native epoll (Linux), NIO fallback elsewhere |

## Architecture

Source packages under `src/main/java/com/example/feed/`:

```text
auth/                       JWT verification and authenticated caller
health/                     Reactive database health probe
post/api/                   Standard REST controllers and response records
post/service/               Operations and input validation
post/model/                 Post record
post/persistence/           Reactive prepared SQL and row mapping
infrastructure/config/      DATABASE_URL and reactive client configuration
infrastructure/http/        Extra listener, shared per-connection server options
infrastructure/json/        Blackbird and millisecond UTC timestamps
shared/error/               API errors and exception mapping
```

Controllers return Mutiny `Uni` responses; nothing blocks an event loop and there is no
worker or virtual-thread dispatch. Every operation runs the specification SQL, one query per
request, in autocommit with native `$1` parameters. Successful writes respond only after the
autocommit transaction completes. Failed writes are never replayed, because the commit may have
succeeded. Exact like row counts distinguish inserted from duplicate likes. JWT signature, `exp`
and claims are verified on every write, before ID/body validation. Nothing is cached across
requests except prepared statements and pooled connections.

## Optimizations and why

- **Fully reactive request path.** One event loop per 0.5 CPU (Quarkus default: 2 × CPUs) serves
  HTTP and database I/O without thread hand-offs, so throughput scales with cores.
- **Pipelined PostgreSQL client** (`PgBuilder.client()`, up to 8 in-flight queries per connection).
  With network latency to a separate database box, each connection keeps several queries in flight
  instead of idling for a round trip.
- **Database connections spread over all event loops** (`PoolOptions.eventLoopSize`). A pool built
  at startup otherwise puts every connection's I/O on a single event loop, which caps scaling.
- **Pool size scales with CPUs** (32 per visible CPU, capped at the challenge's 512 connections).
- **Bounded wait queue, no acquisition timeout.** The queue (4096 per CPU) absorbs short stalls such
  as GC pauses. Vert.x 4.5 leaves acquisition timers running after a rejected acquisition, which can
  flood the event loops with exceptions, so timeouts are disabled and the queue is bounded instead.
- **Less heap per connection**, which matters at hundreds of thousands of keep-alive connections:
  - Netty native epoll transport instead of NIO (no `SocketChannelImpl`, selection keys and file
    descriptor wrappers per connection).
  - WebSocket per-message/per-frame compression disabled on both listeners: the API serves no
    WebSockets, but Vert.x otherwise installs extension handshakers on every HTTP/1 pipeline.
  - Compact object headers (`-XX:+UseCompactObjectHeaders`) when running on JDK 25+.
- **Asynchronous console logging** with a bounded queue that discards on overflow, so a slow log sink
  cannot block event loops during an error burst.
- **JSON**: Jackson with Blackbird; timestamps formatted in UTC with millisecond precision.
- **gzip** for `application/json` when the client asks for it; 90-second idle timeout, accept backlog
  8192 on both listeners.

## Configuration

`DATABASE_URL`, `JWT_SECRET`, `HOST`, `PORT`, `EXTRA_PORT` and `DB_POOL` follow the challenge
contract. Both listeners share the same router, controllers and database client.

| Visible CPUs | Pool (`DB_POOL` default) | Wait queue | Event loops |
|---|---|---|---|
| 4 | 128 | 16,384 | 8 |
| 8 | 256 | 32,768 | 16 |
| 16 | 512 | 65,536 | 32 |

Optional tuning overrides (not needed for scoring): `DB_PIPELINE` (1–16, default 8),
`DB_WAIT_QUEUE` (default 4096 × CPUs), `MAX_RAM_PCT` (default 70), `GC=g1|parallel|zgc`
(default G1). `DB_POOL` above 512 is rejected.

JVM: G1, `-XX:MaxRAMPercentage=70` (per-connection state lives on the heap),
`-XX:InitialRAMPercentage=10`, plus compact object headers on JDK 25+.

## Build and verification

```bash
sudo bash submissions/java-quarkus-deviknitkkr/install.sh   # Ubuntu 24.04, once, as root
bash seed/make-seed.sh
bash test/run.sh submissions/java-quarkus-deviknitkkr
```

`install.sh` installs `openjdk-25-jdk-headless`. `build.sh` builds the Quarkus fast-jar with the
checked-in Maven wrapper into `target/quarkus-app/`; `start.sh` runs it in the foreground and
prefers JDK 25, falling back to JDK 21.
