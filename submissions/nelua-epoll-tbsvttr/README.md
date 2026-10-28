# nelua-epoll-tbsvttr: OpenResty without the nginx

The plan was OpenResty: Lua in nginx. Rule 11 says no Nginx. So this is Lua's syntax without nginx and without a
Lua VM: [Nelua](https://nelua.io), a statically typed language that reads like Lua and compiles to C. The server is
a hand-written HTTP/1.1 server on epoll with its own pipelined Postgres client. libpq only logs in, OpenSSL does
the HMAC and ISA-L the gzip, all called directly from Nelua. No framework and no runtime.

## Stack

| Piece | Version |
|---|---|
| Compiler | Nelua 0.2.0-dev at commit `a584505` (2025-06-24, the latest), built from source by `install.sh` into `/opt/nelua`. Nelua has no versioned releases, so the commit is the pin. It generates C; gcc 13 (Ubuntu 24.04) compiles it |
| HTTP | hand-written, on `epoll` |
| Postgres | hand-written wire protocol client, pipelined. libpq 16 (Ubuntu's `libpq-dev`) only logs in (SCRAM) and prepares the statements |
| JSON | hand-written: a validating scanner for the request body and JWT parts; responses written by hand |
| JWT | hand-written HS256 check on OpenSSL 3.0's `HMAC()`, compared with `CRYPTO_memcmp` |
| Compression | ISA-L 2.31's igzip (Ubuntu's `libisal-dev`), level 0 |

`build.sh` compiles `src/main.nelua` with `nelua --release` into `build/feed`. `start.sh` runs it in the foreground.
The program is built without Nelua's garbage collector: memory is `malloc`/`free`.

## Process model

- **One worker process per CPU** (`nproc`, so the affinity mask decides; `WORKERS` overrides it), started with
  `fork()`. Each worker runs its own epoll loop on one thread and has its own Postgres connections. The parent only
  waits; workers die with it (`PR_SET_PDEATHSIG`). If a worker dies, the others keep serving.
- **Every worker opens its own listening socket per port** with `SO_REUSEPORT`, so the kernel spreads new
  connections evenly over the workers. Both ports are served by every worker.
- **Batching window:** after a loop pass that handled fewer than 32 events, the worker sleeps 2 ms (`BATCH_US`), so
  the next pass serves more requests per wake-up and per Postgres round trip. A busy worker never sleeps. This
  trades a few milliseconds of latency for CPU (the idea is from `c-epoll-unorig-username`).
- **Idle connections are never closed** by the server (the spec asks for at least 75 s).

## HTTP

- A request is parsed straight from a 64 KB read buffer shared by the worker. Only bytes left over (a partial
  request, or requests pipelined behind one that waits for Postgres) are copied into a buffer of the connection,
  freed again once used.
- Responses are written with one `send()`. What the socket doesn't take is kept until `EPOLLOUT`.
- Handled: keep-alive, pipelining (in order), `Expect: 100-continue`, `Connection: close`, HTTP/1.0. Not handled:
  chunked request bodies (400 and close).
- **gzip** for bodies of 1 KB or more when the client accepts it, which in practice means the feed: igzip level 0
  turns a 3.6 KB feed into ~1 KB in ~6 µs here, against ~10 µs for libdeflate level 1 (which compresses to 0.7 KB).

## Database

- **libpq logs in and prepares the statements, then lets go.** Every statement is SPEC.md's SQL, prepared once per
  connection. After that, the worker speaks the protocol itself on the socket: `Bind` (text parameters, binary
  results) and `Execute` per query, queued per connection and sent with **one `send()` per connection at the end of
  each loop pass**.
- **Reads and writes use separate connections.** A quarter of a worker's connections (at least one) take the writes.
  - **Reads:** one `Sync` per batch, so Postgres answers the whole batch at once. A query is done at its
    `CommandComplete`. If one fails, Postgres skips the rest of the batch, and those queries fail too.
  - **Writes:** every query has its own `Sync`, so each is its own transaction. The response waits for its
    `ReadyForQuery`, which comes after the commit.
- **256 connections in total** (`DB_POOL`), 16 per worker on 16 cores. A query goes to the first connection with
  fewer than 8 queries in flight, so a pass touches few connections, which means few `send()` calls.
- Results come back in binary: ids and `like_count` as integers, `created_at` as microseconds since 2000-01-01 UTC.
  The server turns that into ISO 8601 truncated to milliseconds, without any time zone setting.
- Like on a missing post: `foreign_key_violation` (`23503`) becomes 404 `post not found`.
- **When Postgres goes away,** the queries on that connection fail (`/health` 503, everything else 500). The worker
  reconnects on a later request, at most once a second. Reconnecting is the one blocking call, with a 2 s
  `connect_timeout`.

## Memory

An idle connection is its socket in the kernel plus a ~150-byte struct in the worker. At 125,000 open connections
the server's RSS was 48 MB.

## Results

`test/run.sh`: **passed=42 failed=0** on Ubuntu 24.04 against `postgres:18`, both on aarch64 (Docker on an Apple
Silicon Mac) and on x86_64 (the same, emulated with Rosetta). On both, `install.sh` builds Nelua from source.

Also checked by hand: `created_at` `.999999` comes out `.999Z`; gzip on the feed unzips to the plain body (on both
architectures); a single post isn't gzipped; the full API on `EXTRA_PORT`; `Expect: 100-continue`; 200 requests over
one connection; 2,000 requests 200 at a time; a connection idle for 80 s still answers; 503/500 while the database
is unreachable, then recovery.

### Local load tests (not the benchmark hardware)

Same setup as `typescript-bun-tbsvttr`, `elixir-bandit-tbsvttr` and `pony-stallion-tbsvttr`: Docker on an Apple
Silicon Mac, the server's container pinned to N cores, Postgres and `mload` on their own cores. The CPU numbers are
the whole container (cgroup), including the kernel's network work for its sockets.

**The scaled-down million:** 125,000 users on 2 cores is the box's million on 16 cores, 3 min hold.

| Server | Result | Server CPU (hold) | Server RSS |
|---|---|---:|---:|
| **Nelua** | **pass**: p95 6.7 ms, p99 13.1 ms, 0 errors | 0.23 cores | 48 MB |
| Bun | pass: p95 3.9 ms | ~0.45 cores | 167 MB |
| Node example | pass: p95 3.2 ms | 0.84 cores | 1,011 MB |
| Pony | fail: p95 214 ms, p99 1.63 s | 1.80 cores | 1,678 MB |
| Elixir | fail: p95 788 ms, p99 2.96 s | 1.38 cores | 3,972 MB |

The one bad 5-second window of the Nelua run (p95 280 ms at ~75 s) came when `mload` reached its memory limit and
collected its own garbage. The other runs show the same window.

**The rate per core of a million on 4 cores:** one core, 20,000 users at `-gzip -think-scale 0.2`, ~10,500 req/s.
What each step bought:

| Version | p95 | User | System | Total |
|---|---:|---:|---:|---:|
| libpq, one query at a time per connection | 0.6 ms | 0.135 | 0.289 | 0.424 cores |
| own protocol client, pipelined | 0.5 ms | 0.121 | 0.238 | 0.359 cores |
| + one `Sync` per read batch, igzip | 0.6 ms | 0.092 | 0.227 | 0.319 cores |
| + 2 ms batching window | 6.4 ms | 0.079 | 0.159 | **0.239 cores** |

**A worker's share of a million on 4 cores:** one core, 250,008 idle keep-alive connections (each made one
`/health` request first) plus 20,000 `mload` users at ~10,500 req/s.

| Open connections | CPU | RSS | p95 / p99 |
|---:|---:|---:|---|
| 20,000 | 0.240 cores | 18 MB | 7.1 / 16.9 ms |
| 270,000 | 0.202 cores (user 0.054, system 0.147) | 62 MB | 8.0 / 23.7 ms |

- The idle connections cost no measurable CPU. With all 250,000 open and no load, the server used 0.00 cores and
  56 MB: there are no timers.
- 16 connects failed during the ramp (0.002%). The server's kernel counted no listen overflows or drops, and the
  VM's conntrack dropped nothing. The SYNs were lost in Docker's network.

**On the box:** at a million users on 4 cores, each worker has 250,000 connections and serves ~10,500 req/s, the
load of the last table. That was 0.20–0.25 of a core here. A box core is about 1.7× slower (measured with the
Node example), so that is ~0.4 box cores per worker, ~1.5 of the 4 cores. Server memory would be ~250 MB, plus
the kernel's socket buffers. I expect it to hold the million on 4 cores.

### Compared with other submissions (same local setup)

To see where this stands, I ran the other compiled submissions on the same harness. All of them ran in one
container (Ubuntu 24.04 aarch64, Docker on Apple Silicon, seccomp off so io_uring works), against the same Postgres
container, built with their own `build.sh`. Every one passed `test/run.sh` 42/42 there first.

- **Settings:** each server ran with its defaults (pool sizes, gzip), with one exception.
  `c-iouring-einhornimmond` sizes its threads by `sysconf(_SC_NPROCESSORS_ONLN)`, which counts the VM's CPUs
  rather than the container's, so it got `SERVER_THREADS` set to the cores given.
- **Toolchains:** the `install.sh` of the Zig and Crystal submissions fetch x86_64 toolchains, so I installed the
  same versions for aarch64 from the official downloads, checksums verified.

**1 core, 20,000 users at `-gzip -think-scale 0.2` (~10,500 req/s, a worker's rate at a million on 4 cores).**
CPU is the container's (cgroup) over 30 s of the hold, including the kernel's network work.

| Submission | CPU | User / system | p95 / p99 |
|---|---:|---|---|
| nelua-epoll-tbsvttr (this one) | **0.240** | 0.071 / 0.168 | 7.1 / 16.9 ms |
| zig-iouring-harshitkhandelwal208 | 0.299 | 0.110 / 0.189 | 4.0 / 5.1 ms |
| c-epoll-unorig-username | 0.300 | 0.101 / 0.199 | 10.8 / 14.3 ms |
| cpp-io_uring1m-mattiasca06 | 0.309 | 0.082 / 0.227 | 20.9 / 109.9 ms |
| c-iouring-einhornimmond | 0.313 | 0.127 / 0.186 | 5.9 / 25.2 ms |
| rust-tokio-unorig-username | 0.420 | 0.189 / 0.231 | 20.9 / 127.6 ms |
| crystal-epoll-faustinoaq | 0.432 | 0.131 / 0.300 | 0.7 / 2.8 ms |
| rust-tokio-dadujex | 0.559 | 0.198 / 0.362 | 25.2 / 54.2 ms |
| rust-tokio-cknutson12 | 0.570 | 0.218 / 0.352 | 14.6 / 41.0 ms |

**2 cores, 125,000 users at `-gzip -think-scale 2.5`, 3 min hold** (the five closest from the first table):

| Submission | Result | CPU (hold mean) | RSS |
|---|---|---:|---:|
| c-epoll-unorig-username | pass, p95 7.8 ms, p99 18.5 ms | 0.236 | 8 MB |
| cpp-io_uring1m-mattiasca06 | pass, p95 1.6 ms, p99 12.9 ms | 0.237 | 95 MB |
| nelua-epoll-tbsvttr (this one) | pass, p95 7.3 ms, p99 62.9 ms | 0.239 | 48 MB |
| c-iouring-einhornimmond | pass, p95 4.0 ms, p99 13.5 ms | 0.243 | 80 MB |
| zig-iouring-harshitkhandelwal208 | pass, p95 24.0 ms, p99 214 ms | 0.272 | 21 MB |

What I read from this:
- **At a worker's rate of a million on 4 cores, this one used ~20% less CPU than the next ones.** It had both the
  lowest user and the lowest system time. Three runs of it gave 0.239, 0.251 and 0.240.
- **At the lower rate of a million on 16 cores, the top four are even:** the differences are within run-to-run
  noise (~5%).
- **My explanation, not measured in the others:** the batching window (fewer wake-ups) and one `Sync` per read
  batch (Postgres answers a batch in one go: fewer packets both ways). Together with igzip, they took this server
  from 0.36 to 0.24 cores ("What each step bought" above).

Limits of this comparison:
- **ARM, not the x86 box.** Some of these submissions have x86 fast paths that did not run here: a BMI2 gzip loop,
  `-march=x86-64-v2`, ISA-L's AVX code (which this one also uses).
- **Docker's networking** inflates system time.
- **One run per submission** for the others.

The box may rank them differently.
