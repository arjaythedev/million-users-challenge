# java-epoll-buianhtai

Java 27, Linux epoll through the Foreign Function & Memory API, and a PostgreSQL v3 wire client. No framework,
third-party driver, JNI library or runtime dependency. This replaces a local Quarkus/Vert.x implementation.

## Build and run

| Tool | Version |
|---|---|
| JDK | Eclipse Temurin 27.0.1.0.2+1; `install.sh` pins the Linux x86_64 archive and verifies SHA-256 |
| Compilation | `javac --release 25`; run with the pinned Java 27 JDK |
| Database | PostgreSQL 18 on a separate machine for the official benchmark |

```bash
sudo bash install.sh
bash build.sh
DATABASE_URL=postgres://feed:password@db:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

`install.sh` is for Ubuntu 24.04 x86_64. The code also supports Linux aarch64 for local testing with a matching
JDK. It does not run on macOS: `epoll` and the socket constants are Linux-specific. The executable is built from
source as `build/server.jar`; no build output is submitted.

## Design and tradeoffs

- One platform thread/event loop per available processor, each with its own epoll set and `SO_REUSEPORT` listeners
  on both ports. The JVM follows the CPU affinity mask used for the 4-/8-/16-core runs.
- Idle client state holds no input/output buffer. Per-loop native read/send buffers are reused. Only fragmented
  requests, pipelined bytes waiting on a result, or partially sent responses allocate per-client buffers.
- The fd table has one slot per possible descriptor (8,388,608, matching the service's `LimitNOFILE`), and each
  connection has an owning loop; stale events for an fd reused by another loop are ignored. If `accept` runs out
  of descriptors the loop pauses 1 ms instead of spinning. Client input pauses while a response is backpressured,
  preserving ordering and limiting buffering.
- PostgreSQL sockets use the same epoll loop. Startup supports trust, cleartext password and SCRAM-SHA-256,
  including server-signature verification. The benchmark's SCRAM authentication is exercised locally.
- The exact spec queries are prepared per connection. Parameters and results use the binary protocol; JSON and
  UTC millisecond timestamps are generated in Java, not in SQL. Reads choose the ready connection with the fewest
  requests in flight and pipeline independent Bind/Execute/Sync units.
- Each loop has one writer. Pending writes form an implicit transaction ending in one Sync; responses wait for
  ReadyForQuery after commit. A failed group is retried as separate transactions, so a missing post does not lose
  successful neighboring writes. Lost database connections fail outstanding work and reconnect after one second;
  writes with an unknown commit outcome are not automatically replayed.
- HS256 signatures and numeric expiration are verified on every authenticated request with a loop-local JCA Mac.
  There is no token, row, query-result or response cache.
- JSON responses are written directly into reusable byte buffers, including UTF-8 string escaping. Feed-sized
  responses use a reusable JDK Deflater (level 1) with a gzip header and CRC32 trailer.
- This is **not zero-copy**: socket buffers are copied into reusable parsing buffers, and generated responses
  are copied into native send buffers. Epoll batches readiness notifications; it does not eliminate copies.
- Wake-up batching: when a wake-up delivers fewer than `BATCH_MIN` events, the loop pauses `BATCH_MICROS` after
  sending responses and database writes, so the next wake-up carries more events. This trades latency (about
  5-9 ms more at p95 in the local runs below) for fewer wake-ups and syscalls per request.
- No HTTP idle timeout. Keep-alive, ordered HTTP pipelining, fragmented requests, `Connection: close` and
  `Expect: 100-continue` are handled. Chunked request bodies receive 400; bodies over 16 KB and unusual methods/IDs
  are outside the challenge's tested contract.

The explicit protocol handling gives control over memory and batching but is considerably harder to maintain
than Vert.x/Netty. Neither epoll nor these local measurements guarantees a better leaderboard score.

## Optional settings

| Variable | Default | Meaning |
|---|---|---|
| `LOOPS` | JVM available processors | Event loops |
| `DB_POOL` | 64 | Total read connections, rounded up across loops; plus one writer per loop |
| `GZIP_LEVEL` | 1 | 0 disables gzip; otherwise JDK Deflater level |
| `GZIP_MIN` | 1024 | Minimum JSON body size for gzip when requested |
| `BATCH_MIN` | 32 | A wake-up with fewer events than this is followed by the pause below |
| `BATCH_MICROS` | 2000 | Pause in microseconds after a small wake-up; 0 turns batching off |
| `HEAP` | 2g | Fixed, pre-touched ParallelGC heap in `start.sh` |

Keep `LOOPS * (ceil(DB_POOL / LOOPS) + 1)` at or below the challenge's 512 database-connection limit.

## Verification

The upstream test runner uses a fresh `feed_test` clone of `feed_seed` and validates the API on the primary port,
with a health check on the second port:

```bash
PGHOST=db PGUSER=postgres PGPASSWORD=password bash test/run.sh submissions/java-epoll-buianhtai
```

Additional regression checks use Python 3's standard library, with no Python packages required:

```bash
PGHOST=db PGUSER=postgres PGPASSWORD=password bash submissions/java-epoll-buianhtai/test/run.sh
```

These exercise malformed numeric JSON, nonfinite JWT expiration, fragmented/pipelined HTTP ordering, gzip,
backpressured responses and commit/read-back behavior when concurrent valid writes mix with missing-post errors.
Both runners write only to the throwaway `feed_test` database; do not point them at an existing database to keep.

Observed on Ubuntu 24.04 arm64 and emulated amd64 with Temurin 27 and PostgreSQL 18 using SCRAM authentication:
all 42 upstream checks and all six protocol regressions passed on both architectures. A separate recovery smoke
observed health change from 503 to 200 after restoring the schema, then from 200 to 503 to 200 when
database connections were disabled and restored.

## Benchmark status

Official 16-, 8- and 4-core scores and million-connection behavior are **unmeasured**. Local shared-machine tests
are comparisons, not estimates of those scores. No cloud resources were ordered or paid for.

Local comparison (one core pinned, 10,000 users with decreasing think time, Postgres 18 and the load generator in
the same Docker VM over loopback, identical JVM and pool settings for all servers, `mload -gzip`):

| Load | Batching off | Batching on | `java-epoll-stohirov` |
|---|---|---|---|
| about 8.9k req/s | 30.5 µs CPU/request, p95 3.3 ms | 22.7 µs, p95 11.6 ms | 22.0 µs, p95 10.9 ms |
| about 17.5k req/s | 26.8 µs, p95 3.7 ms | 16.7 µs, p95 12.4 ms | 15.1 µs, p95 11.5 ms |
| about 29k req/s | 22.6 µs, p95 9.8 ms | 15.0 µs, p95 15.2 ms | 14.3 µs, p95 13.7 ms |

All runs had 0% errors. Idle connections cost about 250 bytes of process RSS each at 10,000 connections (a Quarkus
version of this server: about 9.5 KB). These runs use one core and loopback, stop below saturation, and do not
include kernel socket memory, so they say nothing certain about the official scores.

## Design references

This entry was designed after reading the public READMEs of `rust-tokio-cknutson12` and `java-epoll-stohirov`. For
the Rust entry I also read its JWT validation module and excerpts of its main, HTTP and database code; for the
Java entry only its README, and later its launch script. Taken from those designs: per-loop `SO_REUSEPORT`
listeners, shared per-loop buffers, pipelined reads with a separate writer, group commit with one-by-one retry of
a failed group, gzip for the feed only, the setting names and defaults `LOOPS`, `DB_POOL`, `GZIP_LEVEL` and
`GZIP_MIN`, and, from `java-epoll-stohirov`, the wake-up batching mechanism with the names `BATCH_MIN` and
`BATCH_MICROS` and their defaults (32 events, 2000 µs). No code was copied: this entry's code was written
independently in Java. The reference Java entry is used only as an unmodified local comparison and is not included
here. The Gregorian date calculation follows Howard Hinnant's civil-from-days algorithm.

MIT, under the repository's [license](../../LICENSE).
