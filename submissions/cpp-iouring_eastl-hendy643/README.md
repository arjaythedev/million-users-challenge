# C++ + EASTL + io_uring

A C++23 implementation of the feed API in `SPEC.md`. One io_uring event loop per core serves the keep-alive connections, and our own PostgreSQL protocol client talks to the database.

## How

- **Build:** gcc, CMake presets and Ninja. `-O3 -march=native -ffast-math -flto=auto`, profile-guided (`build.sh` trains on a canned request mix). `install.sh` installs only the compilers, CMake, Ninja, git and `liburing-dev`.
- **Dependencies:** CPM.cmake fetches EASTL, StringZilla, libdeflate and Mbed TLS (HMAC-SHA256 for the JWT check), pinned and linked statically. There is no PostgreSQL client library. The binary needs only glibc and libm.
- **Event loop:** io_uring with multishot accept, multishot receive into provided buffers (idle connections hold none), asynchronous sends, and a `SO_REUSEPORT` listener per loop. One loop per usable CPU (`sched_getaffinity`).
- **Database:** our own wire-protocol client (`src/pg_wire.*`, `src/pg_pool.*`): SCRAM-SHA-256, prepared statements, binary parameters and results, 128 connections in total with up to 32 queries pipelined on each. The queries of one loop iteration share a Sync, so they commit together and no answer leaves before its commit. A rejected statement rolls the batch back and the others are replayed alone.
- **Settings:** every tunable is a constant in `src/tuning.hpp`; nothing is configured from outside and no Postgres setting is touched.
- **Strings:** StringZilla; EASTL for containers.

```bash
sudo bash install.sh
bash build.sh     # configure, build, unit tests
bash start.sh     # needs DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT
```

## Performance

Pass = p95 < 500 ms, p99 < 1000 ms, errors < 1%. The scoring runs use think scale 2.5; think scale 0.1 sends 25 times the requests per user.

**Machine 1: AMD Ryzen 9 5900X (12 cores, 24 threads), 16 GB for WSL2**, with the server, a stock Postgres 18 and the load generator sharing all CPUs, so the server had no dedicated cores. Ramp 30 s, hold 60 s, single runs.

| Users / think scale | req/s | p95 / p99 | Pass |
|---|---:|---:|---|
| 50k / 0.1 | 41.6k, 42.8k | 208 / 342 ms, 145 / 227 ms | yes |
| 50k / 0.07 | 47.7k | 359 / 429 ms | yes |

Beyond that the host's CPUs were all busy, so that was the rig's ceiling. At 1M idle connections the server's own memory was 103 MB, and about 3.3 GB including its kernel sockets.

**Machine 2: AMD Ryzen Threadripper 9970X (32 cores, 64 threads), 122 GB, Linux.** Server pinned to 16 logical CPUs, Postgres to 8, the load generators to the rest. Hold 60 s.

| Users / think scale | req/s | p95 / p99 | Pass |
|---|---:|---:|---|
| 50k / 0.1 | 44.3k | 2.7 / 4.4 ms | yes |
| 100k / 0.1 | 79.1k | 23.7 / 36.0 ms | yes |
| 125k / 0.1 | 98.0k | 57.0 / 70.2 ms | yes |
| 1,000,000 / 2.5 (16 generators of 62,500) | 25.6k | 2.3 / 3.5 ms | yes |
| 1,500,000 / 2.5 (24 generators of 62,500) | 36.5k | 2.5 / 3.6 ms | yes |

At 1M users the server's 16 cores averaged about 8-15% busy and Postgres's 8 cores about 20%. The 1M run used 16 generators because a single load generator address stalls past about 85k connections. The 1.5M run is the most the host's memory holds (about 51 GB for the generators and both ends of every connection); a 2M attempt ran it out of memory.
