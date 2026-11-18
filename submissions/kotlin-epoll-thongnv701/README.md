# Kotlin + its own epoll event loops + its own Postgres client

| | |
|---|---|
| Language | Kotlin 2.4.21 (the `kotlinc` CLI from the official zip, sha256-pinned in `install.sh`), compiled to JVM bytecode |
| Runtime | Eclipse Temurin `jdk-25.0.4.1+1`, official tarball, sha256-pinned in `install.sh` |
| Framework | None. A hand-written HTTP/1.1 server on Linux `epoll`, called through FFM (`java.lang.foreign`), one event loop per core |
| Postgres driver | None. A small v3 wire-protocol client on the same event loops: prepared statements, pipelining, binary results, SCRAM-SHA-256 |
| JSON | Written byte by byte into the response. A small strict parser reads request bodies and JWTs |
| Dependencies | The Kotlin standard library, bundled into the jar by `kotlinc -include-runtime`. Nothing else. No Gradle or Maven |

## Credit

The design and most of the code are a Kotlin port of
[`submissions/java-epoll-stohirov`](../java-epoll-stohirov) (MIT, this repository). The event loop, the Postgres
client, the read packing, the group commit, the gzip encoder and the socket settings all come from it. The port
keeps its defaults, so the two can be measured against each other. The changes are listed under
[What is different from the Java entry](#what-is-different-from-the-java-entry).

## Running it

```bash
sudo bash install.sh   # Temurin 25 to /opt/jdk, kotlinc 2.4.21 to /opt/kotlinc
bash build.sh          # src/challenge/*.kt -> build/server.jar
DATABASE_URL=postgres://... JWT_SECRET=... HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

Linux only, because the server calls `epoll` directly. x86_64 and aarch64 both work.

Optional environment variables, with their defaults:

| Variable | Default | What it sets |
|---|---|---|
| `LOOPS` | the cores the JVM sees | event loop threads |
| `DB_POOL` | 64 | read connections to Postgres in all, split over the loops |
| `DB_WRITERS` | 4 | write connections per loop |
| `PACK_MAX` | 64 | most reads behind one `Sync` |
| `GZIP_LEVEL` | 1 | 0 turns gzip off |
| `GZIP_MIN` | 1024 | smallest body to compress, in bytes |
| `BATCH_MIN` | 128 | a wake-up with fewer events than this waits before the next one |
| `BATCH_MICROS` | 4000 | how long that wait is, in µs. 0 turns it off |

## Design

- One event loop per core. Each loop has its own epoll set, its own `SO_REUSEPORT` listening socket on both ports,
  its own clients and its own Postgres connections. Loops share nothing, so there are no locks and no queues
  between threads.
- A connection is an fd and a small object in a table indexed by fd. Read and write buffers are shared per loop.
  A connection gets its own buffer only while it holds a partial request or a response that the socket did not
  take.
- The SPEC's five statements are prepared once per connection. `id` and `like_count` come back as text and go
  straight into the JSON. `body`, `username` and `created_at` come back binary. `created_at` is formatted from
  its microseconds and truncated to milliseconds.
- All the reads of one loop pass go to one read connection behind one `Sync`. That is one packet to Postgres and
  far fewer packets back. If one read fails, Postgres skips the rest up to the `Sync`, and those reads go out
  again.
- Writes use group commit. While a write transaction is in flight, new posts and likes queue. When it commits,
  the queue goes out as the next transaction. Each response waits for the commit of its own transaction. A
  transaction that has not committed within 10 ms no longer holds the queue: the next one goes out on another
  write connection. A like on a missing post fails its foreign key (23503) and rolls back its batch. The batch
  then goes again one write at a time, so each write gets its own answer.
- A wake-up with fewer than 128 events waits 4 ms before the next `epoll_wait`, so later wake-ups carry more
  events and more queries go to Postgres in one `write`. Responses leave before the wait.
- The listening sockets use `TCP_DEFER_ACCEPT`, `TCP_NODELAY` and a cleared `TCP_QUICKACK`, so the ACK of a
  request rides on its response. `TCP_QUICKACK` is cleared again on a socket whose response took 25 ms or more.
- The feed is gzipped with an own DEFLATE encoder made for one small body at a time. Under load the feed fits in
  one TCP segment.
- JWTs are verified in full on every request: HS256 only, signature first, then `exp`, `sub` and `username`.
- The JVM runs `ParallelGC` with a fixed 2 GB heap that is touched at start.

## What is different from the Java entry

- The code is Kotlin. Every hot path uses primitive arrays and `@JvmField` fields. The compiler flags
  `-Xno-param-assertions -Xno-call-assertions -Xno-receiver-assertions` remove Kotlin's null checks on
  parameters.
- FFM downcalls from Kotlin: `MethodHandle.invokeExact` is signature-polymorphic, and Kotlin takes the call-site
  type from the argument types and the `as` cast. A boxed or `Object` type at such a call site throws
  `WrongMethodTypeException` at run time. `build.sh` reads the bytecode with `javap`. If it finds such a call
  site, it stops the build.
- The gzip encoder reads and writes its little-endian words through heap `MemorySegment` views instead of
  `VarHandle`s.
- io_uring is not ported. The server uses epoll only.

## Local measurements

These numbers compare this entry with the Java entry on the same machine. They do not predict the benchmark boxes,
where the network card and the kernel's network processing are in the path.

Setup: Docker on an Apple Silicon laptop (arm64). mload, the server and Postgres 18 each run in their own network
namespace, with segmentation offloads off so that packet counts match the wire. The server sits on two networks, so
client and Postgres traffic count separately. 2 loops, 20,000 users, `-think-scale 0.125` (about 16,800 req/s),
`-gzip`, 30 s ramp, 120 s hold, counters read over the middle 90 s.

| Run | Segments to / from clients per request | Segments to / from Postgres per request | Server µs per request | Postgres µs per request | p95 | p99 |
|---|---:|---:|---:|---:|---:|---:|
| this entry, run 1 | 1.003 / 2.004 | 0.084 / 1.000 | 14.9 | 25.9 | 11.6 ms | 13.7 ms |
| `java-epoll-stohirov`, run 1 | 1.002 / 2.003 | 0.084 / 1.000 | 14.6 | 26.0 | 11.6 ms | 13.6 ms |
| this entry, run 2 | 1.002 / 2.003 | 0.084 / 1.000 | 14.6 | 25.6 | 11.6 ms | 13.3 ms |
| `java-epoll-stohirov`, run 2 | 1.012 / 2.003 | 0.089 / 1.000 | 15.5 | 27.3 | 11.6 ms | 17.1 ms |
| this entry, run 3 | 1.002 / 2.003 | 0.086 / 1.001 | 14.3 | 25.4 | 11.0 ms | 13.9 ms |

The two entries send the same packets, about 4.09 segments per request, and use the same CPU within the
run-to-run range. The client side is close to its floor of 3.00: the request, the response and the client's ACK.

Correctness checks, all local:

- `test/run.sh`: `passed=42 failed=0` in clean Ubuntu 24.04 containers on arm64 and on x86_64.
- 400 keep-alive connections, each sending 4 pipelined requests at a time (read a post, like it, create a post,
  read the feed). Every response was matched to its own request. Over 40 s, the backends were terminated 4 times:
  2.33M responses, 0 mismatched, and the process stayed up. The requests in flight at each kill got a 500.
- Login with SCRAM-SHA-256, MD5, trust, and a password with %-encoded characters in `DATABASE_URL`.

## Not done yet

- Measurements on the benchmark boxes.
- Tuning against those runs: loop count, readers per loop, pack size, wake-up batching, write window, GC.

## License

MIT, under the repository's [license](../../LICENSE).
