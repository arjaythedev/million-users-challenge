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
the whole container (cgroup), including the kernel's network work for its sockets. The setup, the commands and the
scripts are in [bench/](bench/README.md).

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

These are single runs. Runs on this machine vary by up to ~30% (see below), so read the steps for their
direction more than their exact size.

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

### What to expect on the box

Every entry gets three scores: the most users held for 5 minutes on 16 cores (capped at 1,000,000), on 8 cores and
on 4 (both searched up to 3,000,000). To turn the numbers here into box numbers, I ran the two entries the
leaderboard has scored on this harness too and compared them with their box results on 4 cores:

| Submission | CPU per request here (1 core, ~10,550 req/s) | On the box, 4 cores | Box ÷ here |
|---|---:|---|---:|
| `java-epoll-stohirov` | 29 µs | 70 µs (69% of 4 cores at 39,319 req/s) | 2.4 |
| `rust-tokio-cknutson12` | 54 µs (one run) | 88 µs (79% of 4 cores at 35,938 req/s) | 1.6 |
| this one | 21 µs | 35 to 52 µs by those two factors | |

- **4 cores:** at the 69% that `java-epoll-stohirov` used for its 4-core score, that is 53,000 to 79,000 req/s. A
  million users is ~32,500 req/s, so **~1.6 to 2.4 million users**, if the CPU is what stops it.
- **8 cores:** the CPU alone wouldn't stop it before the 3,000,000 ceiling. Something else will: Postgres or the
  network.
- **The catch:** `java-epoll-stohirov` stopped at ~1.33 million on 8 cores as well, at 40% CPU, so something
  other than its CPU stopped it. Both scored entries already show a p95 of ~200 ms at a million on 16 cores with
  the CPU mostly idle, and 200 ms is Linux's minimum TCP retransmission timeout. My guess is lost packets on the way.
  This one has the fewest packets per request, in and out together, of the submissions I counted (below), which
  should help there. If that limit is the same for everyone, the 8- and 4-core scores will end up closer than the
  CPU numbers say.
- **Memory at the 3,000,000 ceiling:** ~0.7 GB in the server by the 270,000-connection test, plus the kernel's
  sockets.

### Compared with other submissions (same local setup)

To see where this stands, I ran the other compiled submissions on the same harness. All of them ran in one
container (Ubuntu 24.04 aarch64, Docker on Apple Silicon, seccomp off so io_uring works), against the same Postgres
container, built with their own `build.sh`. Every one passed `test/run.sh` 42/42 there first.

- **Settings:** each server ran with its defaults (pool sizes, gzip), with these exceptions, all for ARM or this VM:
  - `c-iouring-einhornimmond` sizes its threads by `sysconf(_SC_NPROCESSORS_ONLN)`, which counts the VM's CPUs
    rather than the container's, so it got `SERVER_THREADS` set to the cores given.
  - `csharp-epoll-xeonfx` declares `struct epoll_event` with x86-64's packed layout, which is wrong on ARM, so I
    changed that one line (`Pack = 4`) for these runs. On the box its layout is right.
  - `csharp-epoll-aot-gichamba` names `linux-x64` as its runtime, so it was published for `linux-arm64` here.
  - `fsharp-kestrel-uring-delneg` links only libdeflate's x86 CPU detection, so its native gzip library got the ARM
    one added, and its package lock (made for x64) wasn't enforced.
- **Toolchains:** some `install.sh` scripts fetch x86_64 toolchains (Zig, Crystal, .NET 10.0.303, .NET 11 RC 3), so I
  installed the same versions for aarch64. The Zig and Crystal downloads were checked against their checksums.
- **Not measured:** `cpp-epoll-dungngo4520` didn't build here. It calls `PQsendPipelineSync`, which is new in
  libpq 17, and Ubuntu 24.04 has libpq 16.

**How far to trust the CPU numbers.** On this machine, the same server under the same load measured anywhere from
0.21 to 0.28 cores over eight runs. It is an Apple Silicon Mac, other work ran on it at times, and the VM's cores
land on faster or slower cores of the host. A single run can't tell apart servers that are within ~30% of each
other. In the second session below the Mac was quieter, and repeated runs agreed within ~10%. The packet counts
further down don't depend on the clock.

**First session. 1 core, 20,000 users at `-gzip -think-scale 0.2` (~10,500 req/s, a worker's rate at a million on 4
cores).** CPU is the container's (cgroup) over 30 s of the hold, including the kernel's network work.

| Submission | CPU (cores) | Runs | p95 / p99 (one run) |
|---|---|---:|---|
| nelua-epoll-tbsvttr (this one) | 0.21–0.28, median 0.24 | 8 | 7.1 / 16.9 ms |
| csharp-epoll-xeonfx | 0.202 and 0.286 | 2 | 2.1 / 4.7 ms |
| rust-uring-unorig-username | 0.238 | 1 | 9.8 / 12.3 ms |
| zig-iouring-harshitkhandelwal208 | 0.299 | 1 | 4.0 / 5.1 ms |
| c-epoll-unorig-username | 0.300 | 1 | 10.8 / 14.3 ms |
| cpp-io_uring1m-mattiasca06 | 0.309 | 1 | 20.9 / 109.9 ms |
| c-iouring-einhornimmond | 0.313 | 1 | 5.9 / 25.2 ms |
| c-alexandretoscano04 | 0.378 | 1 | 0.7 / 3.1 ms |
| rust-tokio-unorig-username | 0.420 | 1 | 20.9 / 127.6 ms |
| crystal-epoll-faustinoaq | 0.432 | 1 | 0.7 / 2.8 ms |
| rust-tokio-dadujex | 0.559 | 1 | 25.2 / 54.2 ms |
| rust-tokio-cknutson12 | 0.570 | 1 | 14.6 / 41.0 ms |

**First session. 2 cores, 125,000 users at `-gzip -think-scale 2.5`, 3 min hold** (the closest from the first
table):

| Submission | Result | CPU (hold mean) | RSS |
|---|---|---:|---:|
| c-epoll-unorig-username | pass, p95 7.8 ms, p99 18.5 ms | 0.236 | 8 MB |
| cpp-io_uring1m-mattiasca06 | pass, p95 1.6 ms, p99 12.9 ms | 0.237 | 95 MB |
| nelua-epoll-tbsvttr (this one) | pass, p95 7.3 ms, p99 62.9 ms | 0.20–0.31 (4 runs) | 48 MB |
| c-iouring-einhornimmond | pass, p95 4.0 ms, p99 13.5 ms | 0.243 | 80 MB |
| csharp-epoll-xeonfx | pass, p95 6.0 ms, p99 71.6 ms | 0.269 | 19 MB |
| zig-iouring-harshitkhandelwal208 | pass, p95 24.0 ms, p99 214 ms | 0.272 | 21 MB |
| rust-uring-unorig-username | fail here: p95 52 ms, p99 1.2 s | 0.288 | 31 MB |

The p99s of these runs include `mload` collecting its own garbage at its memory limit. `rust-uring-unorig-username`
also had slow windows before that. Its own x86 numbers (a million on 4 cores at ~0.9 cores) are far better, so ARM
or Docker may not suit it.

**Second session: the leaderboard's leader and the newer submissions.** The same 1-core load as the first table,
~10,550 req/s in the hold. The close ones ran twice, in alternating order, with this one in between.

| Submission | CPU (cores) | p95 / p99 (one run) |
|---|---|---|
| csharp-epoll-aot-gichamba | 0.214 and 0.224 | 6.3 / 8.6 ms |
| nelua-epoll-tbsvttr (this one) | 0.223 and 0.229 | 6.5 / 8.9 ms |
| fsharp-kestrel-uring-delneg | 0.247 and 0.273 | 4.7 / 66.8 ms |
| java-epoll-stohirov (first on the leaderboard) | 0.298 and 0.305 | 6.7 / 12.3 ms |
| java-ffm-nnetraga97 | 0.393 | 0.7 / 8.0 ms |
| c-ammarserver-AmmarkoV | 0.489 | 0.9 / 3.4 ms |

**Packets, which don't depend on the clock** (the same 1-core load, 30 s of the hold). These are the container's TCP
segments as `/proc/net/snmp` counts them. Incoming segments that arrive back to back are counted once, after the
kernel merges them (GRO).

| Submission | TCP segments per request, in / out |
|---|---|
| nelua-epoll-tbsvttr (this one) | 2.34 / 1.33 |
| csharp-epoll-aot-gichamba | 2.27 / 2.10 |
| csharp-epoll-xeonfx | 2.24 / 2.18 |
| fsharp-kestrel-uring-delneg | 3.00 / 1.50 |
| java-epoll-stohirov | 2.95 / 2.42 |
| java-ffm-nnetraga97 | 3.01 / 2.48 |
| c-ammarserver-AmmarkoV | 3.03 / 2.47 |

For this one, split by socket (each socket's own counters, before merging):
- **Clients:** 2.02 in and 1.01 out per request. That is the request, the client's ACK and one response segment,
  as low as it goes.
- **Postgres:** 1.15 in and 0.33 out per request, mostly the feed's rows coming back.

Two of the others send more:
- `java-epoll-stohirov` ends every read with its own `Sync` and sends it to the least busy of its connections (from
  its README). So the queries of one loop pass go out on different connections, a packet each.
- `csharp-epoll-aot-gichamba` and `csharp-epoll-xeonfx` don't compress, so the feed takes more segments.

What I read from this:
- **A top group I can't rank on this machine:** this one, `csharp-epoll-aot-gichamba`, `csharp-epoll-xeonfx`,
  `rust-uring-unorig-username`, `zig-iouring-harshitkhandelwal208`, `c-epoll-unorig-username`,
  `cpp-io_uring1m-mattiasca06` and `c-iouring-einhornimmond`. Their differences are within run-to-run variation.
  `fsharp-kestrel-uring-delneg` is close behind.
- **The leaderboard's leader**, `java-epoll-stohirov`, used about a third more CPU than this one in the same session
  and sent ~80% more segments out.
- **Clearly more CPU here:** `c-alexandretoscano04`, `crystal-epoll-faustinoaq`, `java-ffm-nnetraga97`,
  `c-ammarserver-AmmarkoV` and the tokio-based Rust ones, at 1.6 to 2.4 times the top group.

Limits of this comparison:
- **ARM, not the x86 box.** Some of these submissions have x86 fast paths that did not run here: a BMI2 gzip loop,
  `-march=x86-64-v2`, ISA-L's AVX code (which this one also uses).
- **Docker's networking** inflates system time.
- **One run** for most of the others.

The box will rank them.
