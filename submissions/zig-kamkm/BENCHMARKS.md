# Default gzip level 1, 2026-11-06

Parent: `6d93a5ec544c60bca77b50dfc2e827491c376845`. The only code change is the default of `GZIP_LEVEL`, from 0 to 1. SQL, authentication, concurrency and write durability are unchanged, and `GZIP_LEVEL=0` restores the build the official rig measured at `117e287` and `6d93a5e`.

The official four- and eight-core searches both passed 1,285,000 users and failed at 1,330,000, with the app box at 70% and 37% CPU. A limit shared by both core counts points at the network rather than CPU. Level 1 makes each gzipped feed smaller, so an eight-core run at this revision tests whether that limit is bandwidth.

Gzipped `GET /feed` responses after 20 posts in the load generator's format, measured on a fresh seed clone at this revision, including the 143-byte response head:

| `GZIP_LEVEL` | gzip body | response | TCP segments (MSS 1448) |
|---:|---:|---:|---:|
| 0 | 960 B | 1,103 B | 1 |
| 1 (default) | 699 B | 842 B | 1 |

Both levels fit one segment once the feed holds the load generator's posts (3,541 B uncompressed), so level 1 reduces bytes, not response packets. Only the seeded feed (5,102 B) needs two segments. A packet capture of a different server under 20,000 users found one data-bearing packet per response and no bare ACKs from the server; the remaining packets were the clients' ACKs.

Development measurements of level 1 on four lab cores are in the [pull request](https://github.com/arjaythedev/million-users-challenge/pull/43#issuecomment-6656672118): about 18% more app CPU than level 0 at the same load. On the rig's four-core box, at 70% CPU at 1,250,000 users, the extra CPU may lower that score; `GZIP_LEVEL=0` is the fallback.

Validation on the four-core lab host with a fresh seed clone, at the new default and with `GZIP_LEVEL=0`: the 42-check API suite and `tests/gzip_responses.py` passed. `GZIP_LEVEL=4` is still rejected before startup.

---

# CPU and bandwidth follow-up, 2026-11-02

Parent: `117e2878b1779337154cc39d080bfb43712f2dab`. Optional `GZIP_LEVEL` settings spend more CPU on smaller responses; level 0 remains the default. [COMPRESSION.md](COMPRESSION.md) and [compression-results.json](compression-results.json) record the concurrency investigation, compression measurements, current validation matrix, and source/executable hashes. The maintainer's official scores apply to `117e287`; the compression follow-up has not run on the official capacity rig.

---

# Earlier validation follow-up, 2026-11-02

Baseline: `4ac776ce81c0a83efec752e35ddd81ca00c7245f`. The validation revision `117e287` keeps the default two HTTP loops, 16 total database sessions, SQL statements, authentication, and durable write behavior. Its changes address request framing, buffered-request lifetime, and post-body validation.

## Reproduced failures

The baseline passed the repository's 42 API checks and original regression suites. Additional socket tests exposed the following failures:

- Two pipelined requests in one TCP write lost the second request when the first response released the entire input buffer.
- Conflicting Content-Length headers, Content-Length combined with Transfer-Encoding, duplicate Host headers, and control bytes reached the application instead of receiving a transport error.
- Unicode whitespace was included in the post character count instead of being trimmed.
- A post containing an escaped NUL returned 201 while libpq stored only the prefix before the NUL. A direct database read confirmed that `before\u0000after` had become the six-byte string `before`. The new concurrent-write probe also reproduced this inside a write group.
- Invalid UTF-8 in JSON reached PostgreSQL and returned 500 instead of the malformed-JSON error.

The new framing parser and retained input cursor fix the pipeline failure, support chunked bodies and HTTP continue, and reject ambiguous framing before dispatch. Body validation checks UTF-8 and Unicode whitespace. Both individual and pipelined database writes reject embedded NUL bytes before passing text to libpq.

## Load validation

The app ran alone on the eight-vCPU Ubuntu 24.04.5 host. PostgreSQL 18.6 and the unmodified repository load generator shared the separate ten-vCPU host. Runs were serial, used fresh seed clones, and retained `fsync=on`, `synchronous_commit=on`, and 2 GB shared buffers. The default 16-session pool and two HTTP loops were identical across builds.

An 8,000-user screen used `-ramp 5s -hold 30s -think-scale 0.01 -gzip`, with results including the ramp:

| Build | Requests/s | p95 ms | p99 ms | Failed requests | Peak app MiB |
|---|---:|---:|---:|---:|---:|
| Baseline | 34,848 | 318.62 | 451.35 | 0 | 23.3 |
| Framing/body fixes | 35,213 | 266.37 | 341.60 | 0 | 23.6 |

Both runs passed the latency and error thresholds. This was one trial per build with compressed think times; it verifies the fixes under load and does not establish a new capacity score.

A sustained run requested 100,000 users with `-ramp 60s -hold 300s -think-scale 2.5 -gzip`. It completed 1,449,127 requests with zero failures at 4,025 requests/s, p95 3.34 ms, and p99 16.75 ms. All 100,000 connections were open by the first full-count sample at t=75 seconds and remained open through t=360 seconds. The run passed the latency and error thresholds. Peak sampled application RSS was 29.4 MiB. Application RSS for these runs was sampled every second.

The sustained run is a tested operating point. The earlier million-user latency failure was not rerun, so these fixes do not claim to resolve it or establish maximum capacity.

## Verification

The required Ubuntu 24.04.5 host has eight vCPUs and 16 GB RAM. The second host has ten vCPUs, 12 GB RAM, and Ubuntu 26.04. Both run the pinned Zig 0.13.0 build. `install.sh` runs as root; builds and application tests run as an unprivileged user. The compiler installer reuses an existing matching version, and `build.sh` rejects a different version.

Both hosts pass all 42 repository API checks, freshness, 96 concurrent writes, buffer/reset regressions, the new contract edge cases, and HTTP transport regressions. Both default settings and `DB_POOL=1` pass. On the second host, the Python suites also pass with a send interposer that caps partial writes and returns EAGAIN every seventh nonblocking send. The test runner verifies that the interposer is loaded in the server process.

The eight-vCPU host's disk was nearly full before the builds. Only task-owned build outputs were placed on its executable memory filesystem. This is test infrastructure; the submission performs no kernel or server-wide database tuning.

Executable hashes, load-test results, and validation details for `117e287` are recorded in `validation-results.json`. Earlier measurements below apply to their recorded earlier revisions.

---

# Earlier remote benchmark results

Final measured source: `048ad17a1e0c3948030766b97fa073f7c79915a5`. The adjacent JSON records the deployed executable SHA-256.

Measured on 2026-10-29. The application and PostgreSQL 18.6 ran on the same 10-vCPU, 12-GB host, and a separate 8-vCPU, 16-GB host ran the load generator. Network RTT between the two hosts is about 0.2 ms. These are development measurements, not an official leaderboard score.

Every run uses a fresh clone of the same seed: 50,000 users, 500,000 posts, and 2,016,005 likes. The SQL, schema, data, 20,000 signed seed tokens, and database configuration are shared across candidates. Durability remains enabled: `fsync=on` and `synchronous_commit=on`. PostgreSQL uses `shared_buffers=2GB`, `effective_cache_size=8GB`, `random_page_cost=1.1`, `effective_io_concurrency=200`, `jit=off`, `enable_memoize=off`, `max_connections=600`, `checkpoint_timeout=15min`, and `max_wal_size=8GB`.

Tests run serially. The app has a 1,048,576 descriptor limit; the load generator has 1,000,000. Sixteen temporary destination IPs and ports 4101/4102 avoid client source-port exhaustion. Both hosts temporarily use `nf_conntrack_max=2097152`. The Go app uses `GOMEMLIMIT=6GiB`; the load generator uses `GOMEMLIMIT=10GiB GOGC=100`. The final runs use the unmodified repository load generator with its original 4096-byte reader and 1024-byte writer. Its executable SHA-256 is recorded alongside each run. Application RSS is sampled every five seconds; reported peaks are the highest observed samples.

The 2,000-user optimization test uses `-ramp 5s -hold 20s -think-scale 0.01 -gzip`. It measures request-processing speed under compressed think times. It does not represent one million simultaneous connections. The 4,000-user comparison uses the same profile to offer more work. Throughput in these short tests includes the ramp. Each table records one trial per build; small differences should not be treated as a statistically established ranking.

The sustained runs use 600,000 actual users and connections, `-ramp 200s -hold 300s -think-scale 2.5 -gzip`. This stays below the published ramp limit of about 3,300 new users/s. Latency and throughput include the entire ramp and hold, as the benchmark requires. The pass thresholds are p95 < 500 ms, p99 < 1,000 ms, and errors < 1%. Each candidate must also open all requested connections.

The C reference is the unmodified `c-epoll-unorig-username` submission at upstream commit `043fd5637b0d292cfe1451c2786bee07fb75f597`. It uses its default 10 HTTP threads and 60 database sessions. All optimized submissions execute a SQL query per request, verify JWTs on every authenticated request, and wait for COMMIT before acknowledging writes.

## Optimization

| Build | Users | Requests/s | p95 ms | p99 ms | Errors | Peak app MiB |
|---|---:|---:|---:|---:|---:|---:|
| Before, uncached | 2,000 | 16,360 | 53.14 | 86.53 | 0 | 19.1 |
| Optimized | 2,000 | 18,741 | 4.88 | 8.02 | 0 | 16.1 |

## Matched throughput test

| Build | Users | Requests/s | p95 ms | p99 ms | Errors | Peak app MiB |
|---|---:|---:|---:|---:|---:|---:|
| Zig | 4,000 | 33,072 | 37.51 | 55.30 | 0 | 24.3 |
| C reference | 4,000 | 31,264 | 110.97 | 345.02 | 0 | 8.1 |

## Sustained connection test

| Build | Users | Requests/s | p95 ms | p99 ms | Errors | Peak app MiB |
|---|---:|---:|---:|---:|---:|---:|
| Zig | 600,000 | 20,816 | 366.24 | 626.79 | 0 | 137.3 |
| C reference | 600,000 | 20,768 | 524.01 | 780.18 | 0 | 31.7 |

Opened 600,000 connections. Benchmark pass: `true`. C reference opened 600,000 connections; benchmark pass: `false`. No interpolation from smaller user counts is used. This is a tested operating point, not a claim that it is the absolute maximum.

## Reproduction

Build `bench/mload` with Go 1.27.1. On the load generator, after configuring the temporary target addresses and loading `tokens.json` from the standard seed:

```sh
ulimit -n 1000000
export GOMEMLIMIT=10GiB GOGC=100
./mload -target "$TARGET_IPS" -ports 4101,4102 -users 600000 \
  -ramp 200s -hold 300s -think-scale 2.5 -gzip \
  -tokens tokens.json -out result.json
```

`TARGET_IPS` is a comma-separated list of 16 addresses on the app host. Both ports serve the same app process. Use a fresh seed clone before each run. Start the app with `DATABASE_URL`, `JWT_SECRET`, `HOST=0.0.0.0`, `PORT=4101`, and `EXTRA_PORT=4102`; keep the other controls above identical across candidates. The compact JSON alongside this report records binary hashes, command parameters, endpoint statistics, and resource measurements. Go uses its pinned 1.27.1 toolchain with cgo disabled. Rust uses 1.94.0, fat LTO, and `-C target-cpu=x86-64-v2` for the remote build. Zig uses 0.13.0 ReleaseFast with pinned static PostgreSQL and ISA-L libraries; its clean Ubuntu 26.04 build also passes the API and write regressions. API and write-regression commands are in the submission README.

## Excluded experiments

Early runs that overlapped database resets or retained stale processes are excluded. The first 600,000-user screens inherited an SSH hard descriptor limit of 524,288; those connection-capacity results are excluded. Later 60-second ramps at 600,000 users were overload screens at 10,000 new users/s, above the published ramp rate. Their latency is not presented as a scoring result. A smaller-buffer load-generator experiment remains separate from the final standard-generator runs.
