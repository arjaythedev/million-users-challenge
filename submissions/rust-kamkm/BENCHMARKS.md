# Four-core capacity, 2026-11-06

Follow-up to `eedc22d`, the Tokio/hyper version scored on the rig at 1,000,000 (max) / 925,000 / 620,000 users on 16 / 8 / 4 cores. On four cores it was CPU-bound (84% app CPU on the rig). This change keeps the same architecture, SQL, transactions, token checks and responses, and removes per-request overhead.

## What changed

| Change | Why |
|---|---|
| mimalloc as the global allocator | glibc `malloc`, `free` and `memcpy` were 28% of samples at 42k req/s. |
| JSON written directly from row values | Rows are no longer copied into `String`s, formatted by `chrono` and serialized by `serde_json` (together about 20% of samples). Text columns are read in place and timestamps formatted from PostgreSQL's microseconds; unit tests compare the escaping with `serde_json` for every ASCII byte and the timestamps with `chrono` for 100,000 values. |
| Built-in fixed-Huffman gzip (`src/fast.rs`) | zlib-rs level 1 was 13% of samples. The encoder is round-trip tested with flate2 for every match length and distance code, window edges and random input. |
| Own request and response types instead of hyper's | Each request no longer copies every header into a `HeaderMap`; the response head and body are written with one write. |
| No `peek` before each request | A connection waits for readiness without a buffer and reads the request's first bytes directly, one syscall fewer per request. |
| 48 read batches in flight instead of 8 (`READ_BATCHES`) | A batch holds its slot until all of its results are in, so the slot count caps the read work in flight regardless of core count. |
| Up to 4 write batches in flight (`WRITE_BATCHES`) | With one batch at a time, writes queued behind each BEGIN/statements/COMMIT cycle (writes p95 699 ms at a million users). |
| JWT key and validation built once | Signature, algorithm and `exp` are still checked on every request. |

All 42 API checks and the freshness, concurrent-write, HTTP transport and contract-edge regressions pass with `DB_POOL=128` and `DB_POOL=1`, before and after the change.

## Results

Development lab, not the challenge rig. The app ran on cores 0–3 of an 8-vCPU VM (`taskset`, RPS on the same cores, the `setup-app-box.sh` network settings applied at runtime), against PostgreSQL 18 on a separate host with a fresh seed copy per run. Load came from the unmodified `mload -gzip`.

Same load, 200,000 users at `-think-scale 0.5` (about 42,000 req/s offered), 70 s ramp and 120 s hold:

| Build | req/s served (ramp + hold) | p95 | p99 | App CPU in hold | Cores 0–3 busy | Peak RSS |
|---|---:|---:|---:|---:|---:|---:|
| `eedc22d` | 25,424 | 3,173 ms | 3,612 ms | 354% | 93% | 645 MB |
| this change, before concurrent writes | 34,897 | 103 ms | 288 ms | 289% | 81% | 257 MB |

One million users at `-think-scale 2.5`, 300 s ramp and 300 s hold, from three generators (420k and 360k users from two remote hosts, 220k from cores 4–7 of the app VM):

| Build | p50 | p95 | p99 | writes p95 / p99 | Errors | App CPU | Peak RSS |
|---|---:|---:|---:|---:|---:|---:|---:|
| one write batch at a time | 3.0 ms | 223 ms | 602 ms | 699 / 1,283 ms | 0.001% (228) | 308% | 1.01 GB |
| this change | 3.6 ms | 221 ms | 535 ms | 241 / 713 ms | 0.002% (344) | 315% | 1.01 GB |

Both million-user runs pass (p95 < 500 ms, p99 < 1 s, errors < 1%), with the hold at about 42k req/s. All errors were `GET /feed` timeouts on the remote generators, which reconnect through lab DNAT. The 8- and 16-core configurations were not measured.

---

# Validation follow-up, 2026-11-02

Baseline: `0d967735ab22a12956e44c9b60ed27ee0b38e685`. This revision changes request validation and database-error classification. It keeps the SQL, pool sizes, read concurrency, write collection window, and compression settings unchanged.

The maintainer's [official benchmark comment](https://github.com/arjaythedev/million-users-challenge/pull/42#issuecomment-6564838862) applies to the baseline commit. The validation fixes below have their own test evidence and do not claim a new official capacity score.

## Reproduced failures

The original revision passed all 42 repository API checks and its existing regressions. Additional real-socket checks reproduced these cases:

- Unknown nested or empty like routes returned an authentication error instead of 404 when no token was present.
- HTTP/1.1 accepted missing or duplicate Host headers, signed Content-Length values, repeated Transfer-Encoding headers, and signed chunk sizes.
- A validly signed token for a nonexistent user incorrectly received `post not found` when liking an existing post. Only a violation of `likes_post_id_fkey` should produce that error; other database failures use the internal-error response.

Route shape now matches before authentication, while authentication still precedes id validation on a matched route. Framing checks reject ambiguous input before dispatch. Database error mapping checks the failed foreign-key constraint.

## Verification

Builds use pinned Rust 1.94.0 with the existing lockfile and release settings. Installation runs as root and builds run as an unprivileged user. Validation covers Ubuntu 24.04.5 on eight vCPUs and Ubuntu 26.04 on ten vCPUs.

Both default settings and `DB_POOL=1` pass the 42-check API suite and the freshness, concurrent-write, HTTP transport, and new contract-edge suites on both hosts. The edge suite also checks valid optional header whitespace, identical Content-Length values, Unicode bodies, embedded NUL handling, invalid UTF-8, and authentication-before-id behavior. Source formatting is checked with the pinned toolchain's rustfmt.

The eight-vCPU host's disk was nearly full, so task-owned compiler outputs were placed on an executable memory filesystem. The submission itself performs no kernel or server-wide database tuning. `validation-results.json` records the test matrix and executable/source hashes.

---

# Earlier remote benchmark results

Final measured source: `deb9d294513c7ceee4f46ae9707982e215282d64`. The adjacent JSON records the deployed executable SHA-256.

Measured on 2026-10-29. The application and PostgreSQL 18.6 ran on the same 10-vCPU, 12-GB host, and a separate 8-vCPU, 16-GB host ran the load generator. Network RTT between the two hosts is about 0.2 ms. These are development measurements, not an official leaderboard score.

Every run uses a fresh clone of the same seed: 50,000 users, 500,000 posts, and 2,016,005 likes. The SQL, schema, data, 20,000 signed seed tokens, and database configuration are shared across candidates. Durability remains enabled: `fsync=on` and `synchronous_commit=on`. PostgreSQL uses `shared_buffers=2GB`, `effective_cache_size=8GB`, `random_page_cost=1.1`, `effective_io_concurrency=200`, `jit=off`, `enable_memoize=off`, `max_connections=600`, `checkpoint_timeout=15min`, and `max_wal_size=8GB`.

Tests run serially. The app has a 1,048,576 descriptor limit; the load generator has 1,000,000. Sixteen temporary destination IPs and ports 4101/4102 avoid client source-port exhaustion. Both hosts temporarily use `nf_conntrack_max=2097152`. The Go app uses `GOMEMLIMIT=6GiB`; the load generator uses `GOMEMLIMIT=10GiB GOGC=100`. The final runs use the unmodified repository load generator with its original 4096-byte reader and 1024-byte writer. Its executable SHA-256 is recorded alongside each run. Application RSS is sampled every five seconds; reported peaks are the highest observed samples.

The 2,000-user optimization test uses `-ramp 5s -hold 20s -think-scale 0.01 -gzip`. It measures request-processing speed under compressed think times. It does not represent one million simultaneous connections. The 4,000-user comparison uses the same profile to offer more work. Throughput in these short tests includes the ramp. Each table records one trial per build; small differences should not be treated as a statistically established ranking.

The sustained runs use 600,000 actual users and connections, `-ramp 200s -hold 300s -think-scale 2.5 -gzip`. This stays below the published ramp limit of about 3,300 new users/s. Latency and throughput include the entire ramp and hold, as the benchmark requires. The pass thresholds are p95 < 500 ms, p99 < 1,000 ms, and errors < 1%. Each candidate must also open all requested connections.

The C reference is the unmodified `c-epoll-unorig-username` submission at upstream commit `043fd5637b0d292cfe1451c2786bee07fb75f597`. It uses its default 10 HTTP threads and 60 database sessions. All optimized submissions execute a SQL query per request, verify JWTs on every authenticated request, and wait for COMMIT before acknowledging writes.

## Optimization

| Build | Users | Requests/s | p95 ms | p99 ms | Errors | Peak app MiB |
|---|---:|---:|---:|---:|---:|---:|
| Before, uncached | 2,000 | 13,699 | 446.89 | 518.82 | 0 | 57.7 |
| Optimized | 2,000 | 18,471 | 8.27 | 11.83 | 0 | 17.6 |

## Matched throughput test

| Build | Users | Requests/s | p95 ms | p99 ms | Errors | Peak app MiB |
|---|---:|---:|---:|---:|---:|---:|
| Rust | 4,000 | 28,089 | 51.07 | 58.70 | 0 | 29.6 |
| C reference | 4,000 | 31,264 | 110.97 | 345.02 | 0 | 8.1 |

## Sustained connection test

| Build | Users | Requests/s | p95 ms | p99 ms | Errors | Peak app MiB |
|---|---:|---:|---:|---:|---:|---:|
| Rust | 600,000 | 20,869 | 282.76 | 508.60 | 0 | 1049.2 |
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
