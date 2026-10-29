# Kotlin/Native, no framework: own HTTP/1.1 server on epoll, own Postgres client

| | |
|---|---|
| Language / runtime | Kotlin 2.4.21, Kotlin/Native (LLVM) for linuxX64: a native executable, no JVM at run time |
| HTTP | own HTTP/1.1 engine on raw epoll (`platform.linux`), one SO_REUSEPORT listener per event-loop thread and port |
| Postgres | own wire-protocol 3.0 client (SCRAM-SHA-256, extended protocol, prepared statements, binary results) |
| JSON, JWT | own: JSON written straight from binary rows, HS256 with a pure-Kotlin SHA-256 |
| Build | Gradle 9.8.2 (pinned SHA-256) + Kotlin Multiplatform plugin 2.4.21 on Ubuntu's OpenJDK 21 (apt), build time only |
| Dependencies | none besides the Kotlin stdlib |

Based on the author's `kotlin-native-luben93` entry of the multicore server challenge (same engine, Postgres client,
JSON and JWT code), reworked for a million mostly idle keep-alive connections.

Passes `test/run.sh`: `passed=42 failed=0` (Postgres 18.6, both with Postgres on loopback and behind a 250 µs
network delay).

## Design

- **HTTP**: N event loops (`HTTP_THREADS`, default = CPUs from `sched_getaffinity`). Each has its own epoll set and an
  SO_REUSEPORT listener on both ports. Idle connections hold no buffers; each response is one write of headers +
  body with Content-Length.
- **Postgres**: each request runs exactly one SPEC statement, prepared once per connection, with binary results.
  - Read lanes (`DB_READ_CONNS`) take everything queued and pipeline it: Bind/Execute/Sync per request, one round
    trip per batch.
  - Write lanes (`DB_WRITE_CONNS`) group-commit (BEGIN; inserts; COMMIT in one round trip) and answer only after
    COMMIT. If one insert fails (a like on a missing post), the batch rolls back and each insert re-runs on its own.
  - Overload safety: queues are bounded (past the cap, requests get 500 at once instead of piling up); a request
    takes its response buffer only when it renders, not while it waits (~0.5 KB per queued request, not 8 KB);
    queued reads whose client already closed the connection are dropped unrun; lane threads survive any error.
- **gzip**: zlib through Kotlin/Native's `platform.zlib`, one stream per thread, for responses of at least
  `GZIP_MIN_BYTES` (1024) bytes when the client sends `Accept-Encoding: gzip`. In practice that means the 5 KB feed.
  `GZIP_LEVEL` (default 1, 0 = off).
- **JWT**: HS256 with a pure-Kotlin SHA-256; signature, alg and exp are checked on every request.
- **Stats**: one line every `STATS_INTERVAL_S` (10 s, 0 = off), from per-thread counters and histograms. It shows
  conns, accepts/s, req/s per endpoint, server p50/p99, Postgres p50/p99, jobs per round trip and per commit, queue
  depths, 4xx/5xx/shed, gzip ratio and µs, MB/s out, partial writes and RSS.

```bash
sudo bash install.sh
bash build.sh
DATABASE_URL=postgres://feed:...@db:5432/feed JWT_SECRET=million-users-challenge HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

Thread and pool sizes default from the CPUs the process may run on (`sched_getaffinity`). The server never pins
threads to CPUs.

## Settings and why

| Setting | Default | Why |
|---|---|---|
| `HTTP_THREADS` | CPUs | one epoll loop per CPU; idle connections cost no thread |
| `DB_READ_CONNS` | 4 x CPUs, 4..64 | each connection pipelines everything queued; 64 never queued at 6.5k req/s with a 250 µs RTT |
| `DB_WRITE_CONNS` | CPUs / 2, 4..8 | group commit: one round trip and one WAL flush for every write queued meanwhile; at least 4 because 2 saturated on the WAL flush in the author's previous lab |
| `DB_READ_QUEUE_MAX` / `DB_WRITE_QUEUE_MAX` | 65536 / 8192 | bounded: past it requests get 500 at once, so overload never wedges the server; far above the ~21k in flight at the knee (42k req/s x 0.5 s) |
| `DB_GATHER_US` / `DB_GATHER_MIN`, `DB_WRITE_GATHER_US` / `DB_WRITE_GATHER_MIN` | 2000 / 16, 2000 / 4 | gather window: a lane that wakes to fewer than MIN queued jobs waits for more, but never longer than the window after the OLDEST job was queued (so the added latency is at most the window). Fuller pipelines and commits mean fewer round trips, syscalls and lane wake-ups per request. Lab, 4 CPUs at 45k req/s (the load of a million users): server CPU per request -13%, Postgres CPU -10%, reads per round trip 1.4 -> 11, tails unchanged; 4-8 ms gained little more. `0` turns it off |
| `DB_OPTIONS` | `-c plan_cache_mode=force_generic_plan` | session-level only (rule 2): no re-planning of the prepared statements |
| `GZIP_LEVEL` / `GZIP_MIN_BYTES` | 1 / 1024 | the 5.1 KB feed shrinks to ~20% in ~20 µs; small post bodies gain little and go out plain |
| `STATS_INTERVAL_S` | 10 | one stats line on stdout per interval (see Design) |
| codegen | the build machine's CPU | `build.sh` asks Kotlin/Native's clang for `-march=native` (CPU name + feature list): the box builds what it runs |

## Measured locally

An i7-13700H laptop (hybrid; about 1.2x faster than the box in an earlier challenge). The server was limited to the
given CPUs with taskset (a local tool only). Postgres 18 ran in Docker on other CPUs, with its data on tmpfs and
behind a 250 µs netem delay. `bench/mload` ran with `-gzip -think-scale 2.5` on the remaining CPUs, all on one
machine.

| Run | Result | Server CPU per request (+ kernel softirq) | Memory |
|---|---|---|---|
| 4 CPUs, 150,000 users (5.7k req/s) | pass, p95 0.7 ms, p99 1.2 ms, 0 errors | 46.6 µs (+ 10.9 µs) | 109 MB |
| 8 CPUs, 150,000 users | pass, p95 0.9 ms, p99 4.2 ms, 0 errors | 60.2 µs (+ 15.6 µs) | 143 MB |
| 262,123 idle keep-alive connections (after one request each) | held | - | 96 MB, ~400 B per connection |

One machine cannot hold both ends of a million connections, and the host's conntrack table stopped at 262,144. So
1M is extrapolated: about 42k req/s x ~58 µs is ~2.4 of the laptop's cores for the server and its network
processing, and ~0.4 GB of memory.
