# Remote benchmark results

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
