# Autoresearch: fsharp-uring-delneg, a frameworkless F# io_uring entry to take #1 on 4 cores

## Objective

Million Users Challenge (arjaythedev/million-users-challenge): beat C# #1 `csharp-epoll-jh-d` (#59: 4c 1,430,000 /
8c 1,360,000) with a new entry `submissions/fsharp-uring-delneg/`, built from our merged #35
(`fsharp-kestrel-uring-delneg`, 4c 1,340,000 / 8c 1,330,000) without Kestrel's host.

What the maintainer's scoring comments say (read 7 Nov), and why CPU alone is the wrong target:

- #35 on 4 cores: the 2-minute staircase passed 1,529,979 users (64.7k req/s, CPU 48%); the 5-minute holds at 1.48M
  and 1.41M failed on timeouts (p99 8.8 s, 1.7 s) at ~50% CPU; "the limit there is the app box's network".
- #59 (C# #1) on 4 cores: passed 1,476,981 for 2 minutes at 70% CPU, failed 1,529,979, held 1,430,000.
- Every scored entry sits at p95 ~ 210-230 ms and p99 ~ 220-260 ms at its score, on 4 and 8 cores alike: about one
  TCP retransmission timeout. 8-core scores are no higher than 4-core ones. The wall is packet loss and recovery on
  the app box's network at ~1.4-1.5M connections, not the server's CPU.
- io_uring entries average ~5% below epoll entries on 4 cores (rough count of ~20 scored entries).

So the work aims at: no stalls and fast recovery at 1M+ connections over 5+ minutes; fewer packets and bytes per
request; smoother sends; then CPU per request (it decides where the scorer's staircase starts, and 4-core runs share
the cores with the kernel's network work).

## Metrics

On the Synteq box (8 vCPU Xeon 8260, 16 GB, Ubuntu 24.04, kernel 6.8.0-117, Postgres 18 on the same box):

- **screen** (`experiments/scripts/screen.sh`, `screen2.sh`): server on 1 vCPU, 20,000 `mload` users at think-scale
  0.25 (~8.4k req/s) + 100,000 idle connections, 2 min hold: server kcycles per request (user + kernel, `perf stat`),
  Postgres kcycles per request, p95/p99; `screen2.sh` adds packets and bytes per request per direction (tc bands on
  `lo`) and netem impairments (`LOSS=`, `NETEM_S2C=`, `NETEM_C2S=`).
- **scale** (`experiments/scripts/scale.sh` + `hold.c`): server on 4 vCPUs, 1,000,000 keep-alive connections ramped
  at 3,300/s, each GETting `/feed` or `/posts/:id` every ~60 s (~16.7k req/s), 5-minute hold after the ramp:
  timeouts, failures, p99 / p99.9 and the worst 10 s window, server RSS.

Primary metric for keep/discard is set per experiment family in the worklog (tails and timeouts at scale first, then
packets/bytes, then CPU); C# #1 is rerun on the same rig as the yardstick.

## Budget

100 experiments (structural first: 1-50), consult/DeepSeek <= $1 in total, finish before 9 Nov 15:00 UTC.

## Files in Scope

`submissions/fsharp-uring-delneg/**` only.

## Off Limits

`bench/mload`, `test/`, the seed, the rig scripts once a family's baseline is measured; Postgres config; the box's
sysctls (the grader's `setup-app-box.sh` set, plus loopback-only netem for impairment tests).

## Constraints

Rules first (`experiments/rules-check.md`). 42/42 on `test/run.sh` for every kept change. No AOT. .NET 11 RC 3.

## What's Been Tried

See `experiments/worklog.md`.
