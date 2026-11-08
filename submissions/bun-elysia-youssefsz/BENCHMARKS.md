# Local benchmark results

Measured on 2026-10-27 against upstream commit
`8f02bc60cb1ac9729ca6a505f4f36f2aa9c5fbb3`. All performance runs used native
ARM64 Linux containers on one laptop. The official server score is unmeasured.

## Admission correction

The original queue allowed 16 active queries and 256 waiting requests per worker
at the tested pool size. Its 250 ms wait deadline and pool-dependent queue size
rejected recoverable bursts before the HTTP request budget was exhausted.

An instrumented full-length replay recorded 18,557 HTTP 500 responses, matching
17,912 queue-full rejections and 645 queue expirations. Observed SQL-operation
durations sometimes reached 350-413 ms, including client and host scheduling.
Those timings do not isolate database execution from event-loop or VM pauses.

The fix keeps SQL concurrency unchanged and separates it from a bounded budget
of 2,048 active or waiting requests per worker. Waiting expires after 500 ms.
FIFO order, cancellation, expiration checks before SQL, and commit before write
responses remain in place. No automatic retries, caches, or durability changes
were added. The shutdown deadline is 20 seconds to cover the bounded phases.

A real PostgreSQL lock regression pauses writes for 350 ms with eight active
writes and 129 queued writes. The original policy completed 8 of 137; the fix
completed and persisted all 137. Short diagnostic controls are retained under
[diagnostics](benchmarks/admission-fix/diagnostics/). Reducing SQL concurrency
alone still produced 1.016% failures; correcting the waiting policy produced
zero failures in the shorter control. These controls are excluded from the
full-run table below.

## All eight repeated runs

The owner's `examples/node` source, lockfile, build script, and start script
were unchanged. Both implementations used four workers, 64 total database
pool slots, a four-CPU application quota, and 3 GiB application memory.
Each workload ran twice per implementation, reversing app order on the second
pass. Every case used a fresh seed copy and a configured five-minute hold.

| Workload                | App  | Run | Successful requests/sec | p95, ms |  p99, ms | Failed / attempted | App CPU, cores | App peak, MiB | Pass |
| ----------------------- | ---- | --: | ----------------------: | ------: | -------: | -----------------: | -------------: | ------------: | ---- |
| 100,000 connections     | Bun  |   1 |                 4,004.7 |   12.31 |   433.74 |  5,014 / 1,447,765 |           0.80 |           629 | Yes  |
| 100,000 connections     | Node |   1 |                 4,008.2 |  567.43 | 2,524.15 |      0 / 1,442,958 |           1.20 |         1,707 | No   |
| 100,000 connections     | Bun  |   2 |                 4,026.7 |    0.92 |     5.61 |      0 / 1,449,627 |           0.62 |           639 | Yes  |
| 100,000 connections     | Node |   2 |                 4,025.0 |    4.08 |   120.16 |      0 / 1,448,988 |           0.98 |         1,317 | Yes  |
| 6,000 accelerated users | Bun  |   1 |                12,044.1 |    6.38 |    77.56 |      0 / 3,974,543 |           1.84 |           287 | Yes  |
| 6,000 accelerated users | Node |   1 |                11,982.9 |   18.88 |   106.64 |      0 / 3,954,369 |           2.30 |           629 | Yes  |
| 6,000 accelerated users | Bun  |   2 |                11,977.5 |   15.63 |   132.74 |     15 / 3,952,584 |           1.87 |           274 | Yes  |
| 6,000 accelerated users | Node |   2 |                11,997.9 |   13.33 |   114.33 |      0 / 3,959,320 |           2.24 |           613 | Yes  |

Bun passed all four cases. Node passed three, with its first connection run
failing latency. A pass uses the current upstream limits: p95 below 500 ms,
p99 below 1,000 ms, and errors below 1%.

The accelerated Bun repeats recorded 15 failures across 7,927,127 requests,
or **0.000189% combined**. Node recorded zero. Mean successful throughput was
12,010.8 requests/sec for Bun and 11,990.4 for Node. The 0.17% difference is too
small to establish a throughput winner on this machine. Bun used 18.30% less
application CPU and 54.89% less mean peak application cgroup memory.

At 100,000 connections, successful throughput was effectively equal, at
4,015.7 requests/sec for Bun and 4,016.6 for Node. Bun used 34.66% less CPU and
58.05% less mean peak memory. Its first run still had **0.346% failures** and
an **11.3% worst nominal five-second failure window**. Its second had zero.
All four connection runs reached 100,000 sampled open connections.

Bun's remaining failure windows corresponded to admission-expiry logs. The
service still sheds work during sufficiently long stalls or sustained overload.
Neither app had an unexpected worker restart or generator OOM kill.

## Measurement details

Throughput includes ramp. Successful throughput subtracts failed requests.
The upstream latency histograms include failures. A failed feed also skips its
follow-up post read in the unchanged generator, so failure rates can change
the realized request mix. Comparisons cover complete implementations, including
compression, database drivers, and overload handling.

CPU is charged to the full application cgroup, including the parent and
workers. One core is 100% of one logical CPU. Memory is peak cgroup memory,
including charged kernel memory and cache, rather than process RSS. Reported
group memory is the mean of the two per-run peaks. PostgreSQL `memory.peak`
is a container-lifetime value and is not reported as per-run memory.

The [comparison JSON](benchmarks/admission-fix/comparison.json) contains
two-repeat means, ranges, CPU cost per attempted and successful request, and
percentage formulas. Latency averages are means of per-run percentiles, not
pooled percentiles. The [CSV](benchmarks/admission-fix/measurements.csv) also
records the worst sampled failure windows and resource pressure.

Connection windows sum nominal elapsed-time ticks present in all four
generators. Their snapshots are slightly offset, so these are not synchronized
wall-clock windows. The upstream generator snapshots histograms and endpoint
counters while requests still run. One result has 67 fewer histogram samples
than endpoint requests. The raw snapshots are retained without correction.

## Environment and limits

- MacBook Air M5 ARM64, 10 cores, 24 GiB RAM, AC power.
- Docker Desktop VM with 10 logical CPUs and about 7.75 GiB RAM.
- Common Ubuntu 24.04 image with Bun 1.4.2 and Node 22.23.3 ARM64.
  The upstream Node installer is x86-only. The same pinned version's official
  ARM64 archive was installed separately and checksum-verified.
- Elysia 1.4.30, Drizzle 0.45.4, jose 6.2.13. Node used the unchanged lockfile:
  Fastify 5.12.7, pg 8.23.1, compress 9.2.2, fast-jwt 6.3.5.
- Bun used `--workers 4 --pool 64`. Node used `WORKERS=4 DB_POOL=16`.
  Bun used gzip level 1; Node's compression defaults were unchanged.
- PostgreSQL 18.6, four-CPU quota, 3 GiB memory, 512 MiB shared buffers,
  2 GiB effective cache, 256 MiB maintenance work memory, JIT and Memoize off,
  and `max_connections=600`. `fsync` and `synchronous_commit` remained on.
- Fresh seed clone per case: 50,000 users, 500,000 posts, 2,016,005 likes.
  Seven seconds of low-load warm-up, versus three minutes officially.
- Unchanged upstream `mload`, compiled with Go 1.25.6, gzip enabled.
  Connections used 100,000 users, think scale 2.5, 60-second ramp, and four
  generators with distinct source IPs and 25,000 users each. Accelerated runs
  used 6,000 users, think scale 0.05, 30-second ramp, and one generator.
- Each generator had a two-CPU quota and 3,500 MiB memory limit.
  Connection generators used `GOGC=25` for both apps, matching the preceding
  valid comparison. Accelerated generators used the default `GOGC=100`.
  The request logic and think times were unchanged. This GC setting differs
  from the original submission's connection run.
- Thirty-second pauses between cases. No builds or tests ran concurrently.
  Execution order and source fingerprints are in the
  [manifest](benchmarks/admission-fix/manifest.json).

The app, database, generators, and other desktop apps shared physical CPU,
memory, storage, and networking. CPU quotas did not reserve dedicated cores.
PostgreSQL was throttled during accelerated runs. VM pressure was sampled
every 30 seconds; short peaks can be missed. Generator cgroup CPU, memory
peaks, and memory events were collected at exit for both apps. These local
runs do not establish maximum throughput or predict a score on the owner's
separate x86 application, database, and generator servers. The accelerated
workload does not represent an equivalent count of official users.

## Earlier results

The [original submission results](https://github.com/youssefsz/million-users-challenge/blob/742c18d98964a87ca4a822a3d207e14a1dcef2b9/submissions/bun-elysia-youssefsz/BENCHMARKS.md)
used bundle `3d73c12a7517568fb8136105d151397594f88044647fb03bb0bdf2628fdec6dd`.
A later unchanged-source comparison observed 5.47% and 4.85% accelerated Bun
failures. Its [results](benchmarks/admission-fix/diagnostics/previous-comparison.json)
and both raw Bun accelerated results are retained. The instrumented old-policy
replay then recorded 0.466%. These different rates show local variability;
the failure diagnosis rests on admission counters and the lock regression.
Historical results and shorter controls are excluded from the eight-run table.

The preceding comparison excluded two generator-OOM attempts and repeated both
apps with `GOGC=25`. None of the eight runs reported here lost a generator.

## Correctness and source identity

The final source passed 21 additional tests and 102 assertions, including the
lock regression, FIFO admission, cancellation, expiry without later writes,
and delayed timer callbacks. TypeScript, formatting, and shell checks passed.
Upstream correctness passed 42 checks on both Linux ARM64 listeners and 42
after a non-root build and execution on Ubuntu x86_64 under emulation.
Emulation was used only for correctness. The unchanged Node example passed
all 42 upstream checks before benchmarking.

Lifecycle checks verified one TCP connection after 80 seconds idle, a blocked
write committing before SIGTERM exit 0, and a worker crash stopping the whole
service with exit 1. Logs are in [validation](benchmarks/admission-fix/validation/).

The eight reruns used the fixed bundle with SHA-256
`17fa383c960ed888e244813883027875cdf7545f03c8795fecdbdd9d4ef705ec`.
The manifest identifies base commit `742c18d` plus the then-uncommitted fix.
That runtime fix is committed as `fed4d10ee00fda5e51b8f592a75827bcfbd70347`.
Production source and bundle hashes matched before and after every run.
Publishing documentation and evidence does not change that runtime bundle.

## Reproduce the workloads

Follow [README setup](README.md#local-benchmark-tools) for the seed, Docker
network, PostgreSQL container, Bun image, and native load-generator binaries.
The comparison image adds the pinned ARM64 Node runtime to that Bun image.
From the repository root, build it and install the unchanged Node example:

```sh
docker build -t million-compare:node22-bun142 \
  -f submissions/bun-elysia-youssefsz/tools/Dockerfile.compare .
docker run --rm -v "$PWD:/challenge" -w /challenge/examples/node \
  million-compare:node22-bun142 bash build.sh
```

From the submission folder, build the fixed app and run the eight cases in
their recorded order. The helper resets only disposable `feed_bench`, starts
the selected app, warms it up, and records results under `results/comparison`.
It uses the same workload commands and quotas as the measured runner. Its
local path setup was adapted for this checkout. Use a quiet machine; do not
run other load tests concurrently. These commands take about 50 minutes.

```sh
bash build.sh
for case in connections:bun:1 connections:node:1 throughput:node:1 throughput:bun:1 \
            connections:node:2 connections:bun:2 throughput:bun:2 throughput:node:2; do
  IFS=: read -r workload app repeat <<EOF
$case
EOF
  if [ "$workload" = connections ]; then
    users=100000; think=2.5; ramp=60s; generators=4
  else
    users=6000; think=0.05; ramp=30s; generators=1
  fi
  python3 tools/compare-local.py "$workload-$app-r$repeat" \
    --implementation "$app" --generator linux --generators "$generators" \
    --workers 4 --pool 64 --users "$users" --think-scale "$think" \
    --ramp "$ramp" --hold 5m || break
  docker stop --timeout=20 million-app
  sleep 30
done
```

Raw result JSON keeps full histograms. Copies compact JSON and replace local
checkout paths with placeholders; measurement values are unchanged. The
[evidence index](benchmarks/admission-fix/README.md) describes the files.
