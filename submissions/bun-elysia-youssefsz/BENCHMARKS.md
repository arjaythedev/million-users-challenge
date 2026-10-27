# Local benchmark results

Measured on 2026-10-27 against upstream commit
`8f02bc60cb1ac9729ca6a505f4f36f2aa9c5fbb3`. These are laptop measurements.
The official score on the challenge servers is unmeasured.

## Results

Both runs used unmodified upstream `bench/mload/main.go`, gzip, and a configured
five-minute hold after ramp. Throughput and percentiles include ramp. A pass here
means the current upstream limits of p95 below 500 ms, p99 below 1,000 ms, and
failed requests below 1%.

| Workload                         |   Users | Think scale | Ramp | Requests/sec |     p95 |      p99 |    Failed / total | Result |
| -------------------------------- | ------: | ----------: | ---- | -----------: | ------: | -------: | ----------------: | ------ |
| Connection test, four generators | 100,000 |         2.5 | 60 s |        4,027 | 0.95 ms |  6.45 ms |     0 / 1,449,606 | Pass   |
| Throughput test, one generator   |   6,000 |        0.05 | 30 s |       12,088 | 2.46 ms | 37.51 ms | 8,516 / 3,988,907 | Pass   |

The connection test reached 100,000 open connections across both ports. Each of
four generator containers opened 25,000 connections from its own source IP.
Peak application cgroup memory was 613 MiB, including the parent, workers, and
charged kernel memory. This is not a process RSS measurement. The application
used 64 PostgreSQL connections in total across four workers.

The throughput test had **0.213% errors**, with HTTP 500 responses from database
admission during bursts. The server bounds queue size and waiting time rather
than accumulating work indefinitely. This run meets the challenge's current error
threshold, but it is not an error-free capacity result. PostgreSQL recorded
10.6 seconds of cgroup CPU throttling during the collection interval.

Both runs used the same built application, SHA-256
`3d73c12a7517568fb8136105d151397594f88044647fb03bb0bdf2628fdec6dd`.

## Environment and limits

- MacBook Air M5, ARM64, 10 CPU cores and 24 GiB RAM.
- Docker Desktop Linux VM with 10 logical CPUs and about 7.75 GiB RAM.
- Ubuntu 24.04 application container, four Bun workers, four-CPU quota,
  3 GiB memory limit, and a 64-connection pool.
- PostgreSQL 18.6 in a separate container on the same laptop, with a four-CPU
  quota, 3 GiB memory limit, 512 MiB shared buffers, 2 GiB effective cache size,
  JIT off and Memoize off. `fsync` and `synchronous_commit` remained on.
- Each Linux generator had a two-CPU quota. Quotas do not reserve dedicated
  physical cores. The app, database, generators, and desktop shared the laptop's
  CPU, memory, storage, and Docker networking.
- Bun 1.4.2, Elysia 1.4.30, Drizzle ORM 0.45.4, and jose 6.2.13.
  The unchanged Go workload source was compiled directly with Go 1.25.6.
- Every case started from a fresh copy of the official seed, with 50,000 users,
  500,000 posts, and 2,016,005 likes. Its content checksum matched upstream.
  Seven seconds of low-load warm-up preceded each measured run.

The owner's method uses a three-minute warm-up, a 16-vCPU x86 application server,
a separate database server, and separate generator machines. Local CPU quotas
and container networking do not reproduce that environment. The accelerated
6,000-user test does not establish an equivalent number of users at the official
think time. These runs tested connection count and throughput separately at
smaller scales; they do not establish one million users or support a reliable
numerical forecast for the official hardware.

## Failed diagnostic run

An earlier 100,000-user test used one generator with the same application bundle,
a 60-second ramp, five-minute hold, and think scale 2.5. It completed 1,411,583
requests with zero errors, but failed latency: p95 2,901 ms and p99 4,070 ms.
Latency rose as connection count passed roughly 64,000 and recovered after all
connections opened. Splitting the load across four source IPs removed that ramp
problem without changing the application.

This supports a generator-capacity explanation. The kernel, scheduler, and GC
contributions were not profiled, so the precise cause remains unconfirmed. The
failed result is retained below and is not counted as a pass.

## Correctness and lifecycle checks

The upstream suite passed all 42 checks on Linux ARM64, with the second port as
the primary target, and after an unprivileged build and execution on Ubuntu 24.04
x86_64 under emulation. The x86 check establishes compatibility under emulation;
all performance measurements above used native ARM64 Linux containers.

The additional suite passed 19 tests and 67 assertions. It covers concurrent
likes, expired JWTs, validation order, Unicode, microseconds, gzip negotiation,
database failures, queued writes expiring without later execution, delayed timer
callbacks, and slow body handling. Real socket and process checks verified the
same TCP connection after 80 seconds idle, SIGTERM draining a blocked write to a
committed 201 response, and worker failure stopping the whole service. Schema
dumps before and after testing matched.

## Evidence and reproduction

The [README](README.md#local-benchmark-tools) describes setup. After building,
run these from the submission folder:

```sh
python3 tools/local-bench.py hold-100000-split --generator linux --generators 4 \
  --workers 4 --pool 64 --users 100000 --think-scale 2.5 --ramp 60s --hold 5m
python3 tools/local-bench.py hold-throughput-6000 --generator linux \
  --workers 4 --pool 64 --users 6000 --think-scale 0.05 --ramp 30s --hold 5m
```

Recorded output includes full histograms and per-generator results. JSON is
compactly formatted, and local checkout paths are replaced with `<checkout>`.
Measurement values are unchanged. Environment files contain commands, parameters,
bundle fingerprints, and cgroup resource counters.

- [100,000-user result](benchmarks/hold-100000-split.json),
  [environment](benchmarks/hold-100000-split.environment.json), and
  [generator 0 log](benchmarks/hold-100000-split-g0.log).
  Generators 1 through 3 have matching JSON and log files in the same directory.
- [Throughput result](benchmarks/hold-throughput-6000.json),
  [environment](benchmarks/hold-throughput-6000.environment.json), and
  [server log](benchmarks/hold-throughput-6000.server.log).
- [Failed single-generator result](benchmarks/hold-100000.json),
  [environment](benchmarks/hold-100000.environment.json), and
  [progress log](benchmarks/hold-100000.log).
- [Unprivileged x86 correctness output](benchmarks/correctness-final-nonroot-amd64.log),
  [second-port correctness output](benchmarks/correctness-final-port2.log),
  [additional tests](benchmarks/additional-tests.log), and
  [lifecycle checks](benchmarks/lifecycle-final.log).
