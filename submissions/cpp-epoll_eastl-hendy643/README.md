# C++ + EASTL + io_uring

| | |
|---|---|
| Language | C++23, clang 20 (Ubuntu 24.04's newest; the stock clang 18 lacks the concepts level libstdc++ 13's `std::expected` needs); release: `-O3 -march=native -ffast-math -flto=thin`, linked with lld, stripped. `FEED_COMPILER=gcc` builds with the distribution's gcc instead (`-flto=auto`), in its own build directory |
| Build | CMake (Kitware's newest) presets + Ninja |
| Dependencies | CPM.cmake (`cmake/CPM.cmake`) fetches every library, pinned to a release tag or a tarball hash in `CMakeLists.txt`, and all of them are built from source and linked statically, the C++ runtime included: EASTL 3.27.01, StringZilla 5.3.2, libdeflate 1.26, Mbed TLS 3.6.7 (HMAC-SHA256 for the JWT check; 4.x would need a lock configured around PSA's global key store), GoogleTest 1.18 (tests). There is no PostgreSQL client library: the wire protocol is our own (`src/pg_wire.*`, `src/pg_pool.*`), SCRAM-SHA-256 included, over plain TCP (the database box runs `ssl = off`). The server binary needs only glibc and libm. `install.sh` installs just the compilers, CMake, Ninja, git and `liburing-dev`, the one library taken from the distribution |
| Event loop | io_uring through Ubuntu's own liburing (`liburing-dev`, 2.5 on 24.04, linked as its static archive): multishot accept, multishot receive into a ring of provided buffers (idle connections hold none), asynchronous sends, one-shot polls only for the database sockets, which the pool reads and writes itself when they are ready. If the kernel refuses to register the buffer ring with EINVAL, which some Ubuntu 24.04 6.8 kernels do for every caller because their backport checks the reserved field the wrong way round, the ring is registered by hand with that field set. |
| Threads | One event loop per thread, each started with `std::async` and joined through its future: `std::thread::hardware_concurrency()` times `kThreadsPerCore`, set in `src/tuning.hpp`. No coroutines: a loop is a state machine over io_uring completions and a request spends its time waiting on a socket or the database, so a coroutine frame per connection would only add allocation |
| Database | our own PostgreSQL protocol client on non-blocking sockets: every loop owns its connections (128 in total, `kDbConnections`), prepared statements, binary parameters and results, up to `kPipelineDepth` = 32 queries in flight per connection. The queries one loop iteration sends to a connection share a Sync, so they commit together, and nothing is answered before the commit; see "Own protocol client" below. A request waits for its answer while the loop serves other connections |
| Strings | StringZilla's; EASTL is for containers |

```bash
sudo bash install.sh
bash build.sh     # cmake --workflow --preset release (configure, build, unit tests)
bash start.sh     # needs DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT
```

## Testing

Unit tests (GoogleTest/GMock, `FEED_BUILD_TESTS`, on by default) run as part of `build.sh`.

The 42-check suite, exactly as CI runs it (Ubuntu 24.04, Postgres 18 with `max_connections = 600`):

```bash
docker compose -f docker-compose.test.yml run --rm --build test    # exit code 0 = all 42 pass
docker compose -f docker-compose.test.yml down -v                  # forget the seed database and the build cache
```

The first run seeds the database (about a minute) and builds every dependency; later runs reuse both.

## Tuning

Every tunable is a constant in `src/tuning.hpp`; nothing is configured from outside (`start.sh` only passes the SPEC.md variables, and no Postgres setting is touched). The session settings (`kSessionSettings`) go in the startup message of our own connections: `jit=off` and `enable_memoize=off` are the ones `box/postgresql.conf` already sets server-wide, `plan_cache_mode=force_generic_plan` and `max_parallel_workers_per_gather=0` skip planning and parallel-query setup for queries this small.

Test rig: Docker on one host with 24 CPUs and 16 GB. The server, a stock-configured Postgres 18 (`max_connections=600`) and the `mload` generator (`-gzip`, two ports) all share those CPUs, so the numbers are pessimistic next to the real two-box setup, and the server has no dedicated cores. Load: ramp 30 s, hold 60 s. Pass = p95 < 500 ms, p99 < 1000 ms, errors < 1%. Think-scale 0.1 sends ten times the requests per user of the real run, so 1M users is about 42k req/s there; think-scale 0.1 with N users is roughly 10N users' worth of requests per connection.

| # | Change | Users / think | req/s | p95 | p99 | Pass |
|---|---|---|---:|---:|---:|---|
| 1 | Baseline: shared listeners (`EPOLLEXCLUSIVE`), 512 DB connections | 10k / 0.1 | 6,248 | 1,139 ms | 1,362 ms | no |
| 2 | Per-thread `SO_REUSEPORT` listeners, 512 DB connections | 10k / 0.1 | 8,874 | 2.4 ms | 3.4 ms | yes |
| | | 20k / 0.1 | 16,354 | 269 ms | 460 ms | yes |
| | | 30k / 0.1 | 18,562 | 1,105 ms | 1,322 ms | no |
| | | 50k / 0.1 | 20,074 | 2,354 ms | 2,679 ms | no |
| 3 | 128 DB connections | 20k / 0.1 | 17,734 | 3.1 ms | 5.1 ms | yes |
| | | 30k / 0.1 | 20,551 | 828 ms | 943 ms | no |
| | | 50k / 0.1 | 23,332 | 1,800 ms | 2,069 ms | no |
| 4 | libpq pipelining (depth 8), one flush per loop iteration, session `jit=off`/`enable_memoize=off` | 30k / 0.1 | 26,580 | 3.3 ms | 6.2 ms | yes |
| | | 50k / 0.1 | 38,941 | 312 ms | 401 ms | yes |
| | | 50k / 0.07 | 40,448 | 812 ms | 933 ms | no |
| | | 50k / 0.05 | 44,857 | 820 ms | 971 ms | no |
| | | 70k / 0.1 | 20,543 | 3.9 s | 4.6 s | no (see below) |
| | | 100k / 0.1 | 18,069 | 5.4 s | 7.4 s | no (see below) |

What each attempt showed:

- **Listener sharing.** With one listening socket woken through `EPOLLEXCLUSIVE`, connections piled onto a few event loops: the server used about one core and Postgres about one backend. One `SO_REUSEPORT` listener per loop spreads them evenly (the same fix the Python example documents). p95 went from 1.1 s to 2.4 ms at 10k users.
- **Connection count.** 512 connections made Postgres burn more CPU for less throughput than 128 (the examples use at most 128 too). 128 raised the 50k-user run from 20k to 23k req/s.
- **Pipelining.** The biggest win. With one query in flight per connection, each of the ~5 connections per loop spent most of its time waiting for a round trip. Pipelining keeps several queries in flight, with a single write per connection per loop iteration. Throughput went from 23k to 39k req/s at 50k users and p95 from 1.8 s to 312 ms. Each query is followed by its own sync, so each is its own transaction, committed before its reply, and a failure (for example the foreign-key violation on a like) does not spoil its neighbours.
- **Session options.** The bench Postgres is stock, with JIT and Memoize on; the box has both off. Setting them on our own connections makes the bench closer to the box.
- **Where it fails.** 70k and 100k users fail on the generator, not the server: mload ran out of ephemeral ports from its single address (`cannot assign requested address`, 150k errors) and used about 20 cores, while the server and Postgres were mostly idle. At 50k users, think-scale 0.07 and 0.05 fail the p95 target at 40-45k req/s with every CPU of the host busy, so that is the rig's ceiling, not a measured server limit. The highest passing level here is 50k users at think-scale 0.1 (38.9k req/s).
- **Not changed:** Postgres configuration, the schema, and the SQL; no caching.
## Would a shared connection pool help?

Each event loop owns its client connections and its own 128/24 database connections. A pool shared between loops (for the input or the output) would need cross-thread queues and wake-ups, so I measured whether the per-loop pools are unbalanced before building one. Temporary counters in `PgPool` recorded how many queries had to wait in `pending_` (every connection of that loop at full depth) and for how long. 50k users, think-scale 0.07 (the case that fails p95 at 812 ms), host saturated:

| Depth | DB connections | req/s | p95 | p99 | Queries that waited | Avg wait |
|---:|---:|---:|---:|---:|---:|---:|
| 4 | 128 | 40,052 | 780 ms | 924 ms | 36% | 79 ms |
| 8 | 128 | 42,400 | 728 ms | 853 ms | 29% | 83 ms |
| 16 | 128 | 42,230 | 679 ms | 853 ms | 25% | 92 ms |
| 32 | 128 | 43,346 | 665 ms | 812 ms | 21% | 98 ms |
| 64 | 128 | 42,688 | 686 ms | 828 ms | 10% | 84 ms |
| 32 | 64 | 43,522 | 721 ms | 862 ms | 45% | 127 ms |
| 32 | 96 | 43,528 | 686 ms | 862 ms | 26% | 151 ms |
| 32 | 192 | 41,882 | 692 ms | 870 ms | 10% | 83 ms |

- The loops are balanced: queries per loop differ by about 4% (147,840 to 161,632 at depth 8), and every loop has a similar share of waiting queries (25-35%). No loop starves while another idles, which is what a shared pool would fix.
- The share of queries that wait ranges from 10% to 45% across these runs while throughput stays at 40-43.5k req/s and p95 at 665-780 ms. The queue is a symptom of saturation, not the cause: the host's CPUs are all busy.
- Conclusion: no shared pool, for clients or for the database. Depth 32 with 128 connections was the best of the sweep, though the differences between depth 8 and 64 are within the run-to-run noise (single runs). The counters were removed again.
## Memory at 1M connections

Measured in the bench container (Ubuntu 24.04, kernel 6.18 on WSL2) with a small client that opens connections from 32 loopback source addresses to ports 3000 and 3001, then sends one gzip `GET /feed` on each. The rig needed `net.core.somaxconn=65535`, `ip_local_port_range=1024 65535`, a 1,048,576 open-file limit and loopback exempted from connection tracking: the default `nf_conntrack_max` of 262,144 stopped the first attempt at 262,024 connections with `connect: Connection timed out`.

| Connections | Server RSS | Kernel + user memory, both ends of the loopback pair |
|---:|---:|---:|
| 0 | 12 MB | 0 |
| 100,000 | 21 MB | 632 MB |
| 500,000 | 58 MB | 3.1 GB |
| 1,000,000 idle | 103 MB | 6.3 GB |
| 1,000,000 after one gzip `/feed` each | 108 MB | 6.3 GB |

So the server's own memory is about 0.1 KB per connection (a `Conn` plus empty SSO strings; request and response buffers exist only while a request is in flight). The kernel's TCP sockets dominate at about 3.2 KB per end, so roughly 3.3 GB for the server side at 1M connections, which leaves most of the 32 GB for the database pool, the request buffers under load and the page cache. Queued data is measured below; any sysctl the box sets differently is not covered.
### Queued data

Worst case for socket buffers: with 1M connections open, every connection sends a gzip `GET /feed` and the client does not read any answer, so each response stays queued in a socket.

| State (1,000,000 connections) | Server RSS | TCP socket buffer memory | Total, both ends |
|---|---:|---:|---:|
| idle | 103 MB | 5 MB | 6.4 GB |
| every response queued, unread | 167 MB | 1.6 GB | 7.8-7.9 GB |
| after the client reads everything | 168 MB | 0 | 6.5 GB |

Queued responses cost about 1.5 KB per connection in the kernel, 1.5 GB for all 1M, and the server's own memory grows by 64 MB (response buffers while the answers are being produced, kept as allocator capacity afterwards). Over the 2 minutes the client held the answers unread the socket memory crept up by about 100 MB as the last responses arrived. On the real network the unread bytes sit in the server's send queues instead, the same order of size. The server side at 1M connections with everything queued is therefore about 3.3 GB + 1.5 GB + 0.2 GB, around 5 GB of the box's 32 GB. A request that waits for a pipelined database answer holds only its small `Conn`; nothing grows with the length of the wait except the client's own queued bytes (capped at 64 KB per connection, `kMaxQueuedInput`).
## Cheaper CPU per request: JSON, libdeflate, PGO

Per request the server spends about 190 µs of CPU, most of it in the kernel (network) and the database client, so these three changes touch a small slice. They are cheap, so they stay.

| Change | Before | After |
|---|---:|---:|
| JSON for a 20-post page (5,187 bytes), microbenchmark | 22.0 µs | 5.2 µs |
| gzip of that body, level 1 | 14-18 µs (zlib) | 7.8 µs (libdeflate) |

- JSON: the timestamp is written straight into the output, runs of plain bytes are appended in one call, and only the bytes that need escaping go through the switch (a 256-entry table). `/feed` reserves the output size up front. The output is byte-identical to the old code (checked in the benchmark, then by the 42-check suite).
- gzip: libdeflate (one compressor per thread) replaces zlib. zlib is gone; the round-trip test decodes with libdeflate's decompressor.
- PGO (clang and gcc): `build.sh` builds an instrumented binary, runs `pgo/train.cpp` (300,000 requests through the real parser, JWT check, JSON and gzip code with canned database answers: 66% `/feed`, 18% single posts, writes, and a few 401/404/400s), merges the profile (`llvm-profdata` for clang), then rebuilds the server's own code with `-fprofile-use` (the unit tests are left out: ThinLTO refuses to link objects with and without a profile). The profile is made on the box and lives in `build/`; nothing is committed and no database is needed. If training fails the build falls back to a plain one; `FEED_PGO=off bash build.sh` skips it. The unit tests and the 42 checks pass on the PGO build.

PGO covers only our own code, which is a few microseconds of the 190, so it is not measured on its own at server level: the rig's run-to-run drift (5-10% on req/s) is larger than anything it can give.

## io_uring instead of epoll

The Linux event loop was rewritten on io_uring (one ring per loop, `SINGLE_ISSUER | DEFER_TASKRUN`, 4,096 submission and 32,768 completion entries). It needs a kernel with io_uring enabled (Ubuntu 24.04's default) and, in Docker, `seccomp=unconfined`, since Docker's default profile blocks it; the compose files set that. There is no epoll fallback on Linux.

Same rig and settings as the Tuning table (Docker, 24 CPUs shared with Postgres and the load generator, ramp 30 s, hold 60 s, single runs):

| Users / think scale | epoll req/s | epoll p95 / p99 | io_uring req/s | io_uring p95 / p99 |
|---|---:|---:|---:|---:|
| 50k / 0.1 | 38.9k | 312 / 401 ms | 42.1k | 188 / 312 ms |
| 50k / 0.07 | 43.3k (fail) | 665 ms / - | 45.3k (pass) | 474 / 621 ms |

Both are single runs on a rig that drifts 5-10% between sessions, so read it as "somewhat better", not as an exact gain. The second row is just inside the limits.

## Own protocol client instead of libpq

libpq is gone. `src/pg_wire.*` is the PostgreSQL protocol (startup, cleartext and SCRAM-SHA-256 login, Parse/Bind/Execute/Sync, DataRow, ErrorResponse), `src/pg_pool.*` the connections. What it does differently:

- **Binary everywhere.** Parameters and results use the binary format: ids are read as big-endian integers and `created_at` is turned from the 8-byte timestamp straight into the text form `format_created_at` expects, so the server never parses numbers or dates from text and no session `TimeZone` matters any more.
- **One Sync per batch.** The queries a loop iteration sends to a connection share a Sync, so Postgres runs them as one transaction and answers once. No answer leaves before that Sync's ReadyForQuery, which comes after the commit, so every write is committed before its response (rule 8).
- **Rollback and replay.** When Postgres rejects a statement it skips the rest of the batch and rolls it back. `pg::batch_fates` then decides per query: reads before the failure keep their answers; writes before it, everything after it, and the failing query itself (unless the error is its own fault: a data exception or constraint violation such as a like on a missing post, or it ran alone) are run again, each in a batch of its own, so a bad query can fail only itself. A connection that dies mid-batch fails its queries with a 500; nothing is retried then, because a write may have been committed.
- **Session settings** in the startup message: `plan_cache_mode=force_generic_plan`, `max_parallel_workers_per_gather=0`, plus `jit` and `enable_memoize` off (see Tuning).
- Name resolution (`getaddrinfo`) blocks the loop; the box's `DATABASE_URL` is an address and a retry happens once a second per dead connection. Only plain TCP and SCRAM/cleartext/trust are supported, which is what the box's `pg_hba.conf` and `ssl = off` need; md5 and TLS are refused with a clear error.

Checked by 31 unit tests (`tests/pg_wire_test.cpp`: URL parsing, every message's bytes, frame splitting, row and timestamp decoding, the batch rules, and the SCRAM exchange against RFC 7677's example), the 42 checks over SCRAM against Postgres 18, and a stress run of 25,600 mixed requests in bursts of 64 (creates, likes, likes on missing posts, feed and single reads) against the rig: 3,859 of them were rejected inside shared batches and every status came out as expected, no 500s, and the 6,380 creates left exactly 6,380 new posts (the newest id was 506,380 after the 500,000 seeded).

Same rig as above, clang build with the own client, single runs (the earlier tables were measured in other sessions, with libpq, so the comparison is loose; the one libpq run at 50k / 0.1 with this compiler had p95 99.5 ms, lower than both runs here, while 0.07 got better):

| Users / think scale | req/s | p95 / p99 | Pass |
|---|---:|---:|---|
| 50k / 0.1 | 41.6k, 42.8k | 208 / 342 ms, 145 / 227 ms | yes |
| 50k / 0.07 | 47.7k | 359 / 429 ms | yes |

With libpq (io_uring, previous session) the 0.07 level passed only just (45.3k req/s, p95 474 ms); here it passes with 100 ms of room.

