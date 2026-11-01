# Optimization follow-up, 2026-11-01

Baseline: `8dc228e1f533d037d7fec39a69fb2c437faab355`. Both candidates use Go 1.27.1 with cgo disabled. Executable hashes, per-endpoint metrics, and individual runs are in [optimization-results.json](optimization-results.json).

The application ran on an 8-vCPU, 16-GB Ubuntu 24.04.5 host. PostgreSQL 18.6 and the unmodified repository load generator ran on a separate 10-vCPU, 12-GB Ubuntu host. Each run started with a fresh `feed_seed` clone containing 50,000 users, 500,000 posts, and 2,016,005 likes. PostgreSQL retained its existing configuration, including `fsync=on`, `synchronous_commit=on`, and 2 GB shared buffers. Both app builds used `GOMEMLIMIT=6GiB` and `DB_POOL=128`. Runs were serial.

The throughput screen uses 8,000 users, `-ramp 5s -hold 30s -think-scale 0.01 -gzip`, on ports 4101/4102. Results include the ramp. This compresses think times to expose request costs; it is not an official connection-capacity score. `GOMAXPROCS` limits Go execution threads. The kernel still has all eight host CPUs, so these are process comparisons, not official 4-core rig measurements.

| Build | GOMAXPROCS | Trial | Requests/s | p95 ms | p99 ms | Failed requests |
|---|---:|---:|---:|---:|---:|---:|
| Baseline | 4 | 1 | 24,268 | 1,891.45 | 2,872.72 | 0 |
| Candidate | 4 | 1 | 35,963 | 146.62 | 173.65 | 0 |
| Baseline | 4 | 2 | 23,909 | 2,048.17 | 2,872.72 | 0 |
| Candidate | 4 | 2 | 36,240 | 148.09 | 184.33 | 0 |
| Baseline | 8 | 1 | 35,517 | 157.20 | 199.60 | 0 |
| Candidate | 8 | 1 | 37,409 | 145.17 | 186.17 | 0 |

Sampled peak application RSS was 117.8 to 118.6 MiB for the four-thread baseline and 146.7 to 155.7 MiB for the candidate. The eight-thread samples were 117.3 and 217.1 MiB. The candidate trades more pooled working memory for lower request overhead. Samples were taken once per second.

The two four-thread trials average about 50% more throughput. The eight-thread result is a single trial and its smaller difference needs more runs before treating it as a stable ranking.

The retained defaults are 16 read workers with batches of up to 64 queries and four write lanes. An earlier 48-worker, 16-query configuration improved read latencies but pushed write p99 above a second. Increasing the batch size alone did not fix that regression. Reducing read concurrency to 16 restored the overall latency thresholds. One writer with 48 read workers and eight readers with batches of 16 also performed worse. Those rejected configurations are recorded separately in the JSON.

The candidate reduces HTTP parsing allocations for ordinary requests, writes JSON directly from PostgreSQL row values, pools response channels and HMAC state, and advances idle deadlines without resetting a timer on every complete request. It still executes the specified SQL for each request, verifies each authenticated token, and waits for durable write completion.

## Sustained connection test

The candidate also passed a 100,000-user run with `-ramp 60s -hold 300s -think-scale 2.5 -gzip`, using both ports. All 100,000 connections were open by the 75-second sample and remained open through the final 360-second sample. It completed 1,449,241 requests with zero failures, 4,026 requests/s, p95 18.32 ms, and p99 23.50 ms. Sampled peak application RSS was 154.4 MiB. This is a tested operating point; the capacity limit remains unmeasured.

## Validation

Both hosts passed `install.sh`, `build.sh`, `test/run.sh` with all 42 API checks, and the freshness, 96-request concurrent-write, and HTTP transport regressions. Both also passed the API and regression tests with `DB_POOL=1`. The Ubuntu 24.04 host passed `go test -race` and the freshness/write/transport regressions against a race-enabled executable. `go vet` passed locally for Linux and on both hosts. Parser regressions cover invalid control bytes and duplicate Host headers; token payload regressions reject a null username.

The upstream GitHub Actions run on the baseline is marked `action_required`, so repository-maintainer approval is still needed for CI. Local remote validation does not replace that approval.

## Reproduction

Build the baseline and candidate with their own `build.sh`. For each trial, create a fresh database from `feed_seed`, set the standard runtime variables, and run the app with `GOMEMLIMIT=6GiB GOMAXPROCS=4`, or `GOMAXPROCS=8`. On the separate load host:

```sh
ulimit -n 1000000
GOMEMLIMIT=10GiB GOGC=100 ./mload -target "$APP_HOST" -ports 4101,4102 \
  -users 8000 -ramp 5s -hold 30s -think-scale 0.01 -gzip \
  -tokens tokens.json -out result.json
```

A temporary firewall rule admitted only the load host to the benchmark ports. The longer connection test expanded only the load generator's source-port range to the challenge setup's `1024 65535`, then restored it. No kernel or database tuning is part of the submission.

---

# Earlier remote benchmark results, 2026-10-29

Final measured source: `43fa94d8b9b383b03d78d209c99671cfffe09c4f`. The adjacent JSON records the deployed executable SHA-256.

Measured on 2026-10-29. The application and PostgreSQL 18.6 ran on the same 10-vCPU, 12-GB host, and a separate 8-vCPU, 16-GB host ran the load generator. Network RTT between the two hosts is about 0.2 ms. These are development measurements, not an official leaderboard score.

Every run uses a fresh clone of the same seed: 50,000 users, 500,000 posts, and 2,016,005 likes. The SQL, schema, data, 20,000 signed seed tokens, and database configuration are shared across candidates. Durability remains enabled: `fsync=on` and `synchronous_commit=on`. PostgreSQL uses `shared_buffers=2GB`, `effective_cache_size=8GB`, `random_page_cost=1.1`, `effective_io_concurrency=200`, `jit=off`, `enable_memoize=off`, `max_connections=600`, `checkpoint_timeout=15min`, and `max_wal_size=8GB`.

Tests run serially. The app has a 1,048,576 descriptor limit; the load generator has 1,000,000. Sixteen temporary destination IPs and ports 4101/4102 avoid client source-port exhaustion. Both hosts temporarily use `nf_conntrack_max=2097152`. The Go app uses `GOMEMLIMIT=6GiB`; the load generator uses `GOMEMLIMIT=10GiB GOGC=100`. The final runs use the unmodified repository load generator with its original 4096-byte reader and 1024-byte writer. Its executable SHA-256 is recorded alongside each run. Application RSS is sampled every five seconds; reported peaks are the highest observed samples.

The 2,000-user optimization test uses `-ramp 5s -hold 20s -think-scale 0.01 -gzip`. It measures request-processing speed under compressed think times. It does not represent one million simultaneous connections. The 4,000-user comparison uses the same profile to offer more work. Throughput in these short tests includes the ramp. Each table records one trial per build; small differences should not be treated as a statistically established ranking.

The sustained runs use 600,000 actual users and connections, `-ramp 200s -hold 300s -think-scale 2.5 -gzip`. This stays below the published ramp limit of about 3,300 new users/s. Latency and throughput include the entire ramp and hold, as the benchmark requires. The pass thresholds are p95 < 500 ms, p99 < 1,000 ms, and errors < 1%. Each candidate must also open all requested connections.

The C reference is the unmodified `c-epoll-unorig-username` submission at upstream commit `043fd5637b0d292cfe1451c2786bee07fb75f597`. It uses its default 10 HTTP threads and 60 database sessions. All optimized submissions execute a SQL query per request, verify JWTs on every authenticated request, and wait for COMMIT before acknowledging writes.

## Optimization

| Build | Users | Requests/s | p95 ms | p99 ms | Errors | Peak app MiB |
|---|---:|---:|---:|---:|---:|---:|
| Before, uncached | 2,000 | 9,492 | 1295.93 | 1403.31 | 0 | 119.7 |
| Optimized | 2,000 | 18,406 | 8.86 | 12.81 | 0 | 47.2 |

## Matched throughput test

| Build | Users | Requests/s | p95 ms | p99 ms | Errors | Peak app MiB |
|---|---:|---:|---:|---:|---:|---:|
| Go | 4,000 | 30,112 | 42.69 | 52.61 | 0 | 66.0 |
| C reference | 4,000 | 31,264 | 110.97 | 345.02 | 0 | 8.1 |

## Sustained connection test

| Build | Users | Requests/s | p95 ms | p99 ms | Errors | Peak app MiB |
|---|---:|---:|---:|---:|---:|---:|
| Go | 600,000 | 20,838 | 366.24 | 706.28 | 0 | 951.6 |
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
