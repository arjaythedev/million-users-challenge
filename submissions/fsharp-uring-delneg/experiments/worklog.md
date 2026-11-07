# Worklog: fsharp-uring-delneg

Rig: Synteq VPS (8 vCPU Xeon Platinum 8260, 16 GB, Ubuntu 24.04, kernel 6.8.0-117: no buffer-ring bug), Postgres 18
and the load on the same box, the grader's sysctls. Not the grader: numbers here are ours and only compare builds.

## 7 Nov: setup

- Kernel: Denis's override (7 Nov) = one 6.8.0 kernel without the -147 buffer-ring bug. Newest 6.8.0 in the archive
  is -149 (affected), so the box boots 6.8.0-117 (GRUB_DEFAULT pinned); the HWE 7.0 kernel briefly installed was
  removed. The fallback (ring -> ring with `resv[0]=1` -> PROVIDE_BUFFERS) stays in the code; `LEGACY_BUFS=1` forces
  PROVIDE_BUFFERS for tests.
- Rig hygiene: nginx (ObjC rig) held :80 after the reboot: stopped. Docker stopped (its NAT table deleted).
- Copied #35's sources, removed Kestrel's host (`Microsoft.NET.Sdk`, plain `main`).

## What the grader says (scoring comments, read 7 Nov)

| entry | 4c held | 4c CPU at hold | 2-min staircase on 4c | 16c p95 / p99 at 1M |
|---|---:|---:|---|---|
| C# #1 `csharp-epoll-jh-d` #59 | 1,430,000 | 72% | passed 1,476,981 (p99 608 ms), failed 1,529,979 | 38.6 / 216 ms |
| Go #65 `go-epoll-trungdlp` | 1,400,000 | 68% | passed ~1.52M, 5-min hold at 1.47M failed | 39.8 / 218 ms |
| ours #35 | 1,340,000 | 47% | passed 1,529,979 (p99 266 ms); 5-min holds at 1.48M / 1.41M failed on timeouts (p99 8.8 s / 1.7 s) | 216 / 236 ms |

All ~20 entries checked show p95 ~210-230 ms and p99 ~220-260 ms at their 4- and 8-core scores, and 8-core scores no
higher than 4-core ones: the wall is the app box's network (loss and TCP retransmission timeouts at ~1.4-1.5M
connections, ~60k req/s), and holds vary run to run. io_uring entries average ~5% below epoll entries.

## Calibration 1: CPU per request, server on 1 vCPU (screen.sh; 100k idle connections; 2 runs each)

| entry | 20k users (8.4k req/s): server kcycles/req, PG kcycles/req, p95 | 36k users (15.2k req/s) |
|---|---|---|
| C# #1 | 102.1 / 238.7 / 6.3 ms | 93.3, 96.3 / 246.5, 270.2 / 7.2, 8.3 ms |
| Go #65 | 97.4, 97.6 / 222.8, 211.6 / 7.9, 7.0 ms | 87.2 / 215.0 / 9.9 ms |
| ours #35 | 64.9, 66.1 / 182.0, 193.7 / 16.1 ms | 62.8 / 198.4 / 19.5 ms |

Our entry costs ~30-35% fewer server cycles per request than C# #1 and Go #65 on this rig, and ~20% fewer Postgres
cycles, yet scored below both: **server CPU on this rig does not predict the board.** CPU stays a secondary metric.

C# #1 r3 (clean, after the r1 runs were spoiled by an orphaned script): 20k 102.3 kcycles/req (PG 222.9), p95 4.9 ms;
36k 98.1 (PG 251.2), p95 7.6 ms. Same picture.

## Calibration 2: segments per request (screen2.sh, loopback MTU 1500, Postgres on its own tc bands; 20k users, 1 vCPU)

Issue #62 relates grader p95/score to TCP segments per request (client + Postgres, both cross the app box's NIC on the
grader: Postgres is on its own machine).

| entry | srv->cli | cli->srv | pg->srv (bytes) | srv->pg | **total** | p95 / p99 |
|---|---:|---:|---:|---:|---:|---|
| C# #1 | 1.001 | 2.001 | 1.053 (1422) | 0.208 | **4.263** | 6.3 / 7.3 ms |
| Go #65 | 1.002 | 2.006 | 1.008 (1369) | 0.137 | **4.153** | 8.1 / 11.8 ms |
| ours #35 | 1.001 | 2.002 | 1.028 (1419) | 0.079 | **4.110** | 18.1 / 24.0 ms |
| v0 (no Kestrel, QUICKACK=0, thin LTO) | 1.003 | 2.008 | 1.027 (1419) | 0.079 | **4.118** | 16.3 / 18.1 ms |

We already send the fewest segments (deep batching: 0.08 Postgres sends per request vs C#'s 0.21). Segment count is
not where #35 loses. What differs is the **blast radius of one lost segment**: with PG_DEPTH=128 fill, a thread's
reads ride one Postgres connection, so one lost Postgres segment stalls 128+ queries for an RTO (>= 200 ms) and
everything queued behind them; C# #1 deals runs of ~16 to the least-loaded connection, so a stalled connection stops
getting work. That matches #35's grader picture (16c p95 216 ms vs C#'s 38.6 ms, at the same network) and its
multi-second timeouts in 5-min holds. Next: Postgres-path loss screens (cal4) and a `PG_RUN=r` dispatch (v3).

## Rig incident: conntrack (7 Nov ~11:50-12:10)

The first 1M-connection holds (sc1m-cs1, sc1m-f35) filled `nf_conntrack` (max 262144; conntrack stays loaded by
Docker's leftover iptables rules) and dropped packets, ssh included: both runs are invalid (fail ~2.1M). The grader's
`setup-app-box.sh` doesn't load conntrack. Fix on the rig: runtime `nf_conntrack_max=4194304`, buckets 1048576 (not
persisted). hi-cs1-r1 ran in the tail of it (only ~127k of 140k sockets up): disregard.

## Calibration 3: Postgres hiccups (CHECKPOINT + VACUUM ANALYZE every 15 s), 40k users, server 4 vCPU

| run | r1 p95 / p99 (worst 5 s p99) | r2 |
|---|---|---|
| C# #1 | (spoiled, see above) | 66.1 / 300.2 (828) |
| #35 | 149.6 / 421.0 (879) | 465.0 / 672.0 (820) |
| v1 | 274.4 / 503.6 (1052) | 285.6 / 455.9 (991) |
| v1 PG_DEPTH=16 | 844.8 / 1116 (1988), fails p95 | 578.8 / 990.6 (1505), fails |
| v1 PG_DEPTH=16 WAIT 64/2 ms | 608.4 / 952.0 (1460), fails | |

Too noisy (#35: 150 vs 465 ms p95 between reps) and confounded: Postgres shares CPUs 4-7 with the load here, so a
shallower dispatch (more Postgres sends, more backends woken) loses to Postgres CPU starvation that the grader's
16-core database box doesn't have. Family dropped; noted only that deep batching is gentler on a starved Postgres.

## Calibration 5: 1M keep-alive connections, server 4 vCPU, 5-min hold, ~16.7k req/s (conntrack fixed)

| entry | fail / timeouts | p50 / p95 / p99 / p99.9 | worst 10 s p99 | retrans |
|---|---|---|---|---|
| C# #1 | 0 / 0 | 4.0 / 6.8 / 9.6 / 54.9 ms | 46.2 ms | 409 |
| #35 | 0 / 0 | 13.7 / 16.3 / 19.3 / 27.4 ms | 27.4 ms | 84 |

Both hold 1M cleanly on loopback (no loss): the rig doesn't reproduce the grader's failure without impairment, which
again points at loss recovery rather than anything in the server at 1M connections.

| #35 MUC_STATS / v3 | | |
|---|---|---|
| v3 (no Kestrel + QUICKACK=0 + thin LTO + REARM_ALL) | 0 / 0 | 13.7 / 16.3 / 19.3 / 23.0 ms, worst 10 s 27.4 ms |
| v3 PG_RUN=16 | 0 / 0 | 9.6 / 11.5 / 11.5 / 19.3 ms, worst 10 s 23 ms |

Conformance v3: 42/42 with GZIP_IMPL=fsharp, LEGACY_BUFS=1, PG_RUN=16 (the default-env run hit errno 98 on port 3000,
a leftover-socket collision right after the 1M hold; rerun pending).

## Calibration 4 (discarded): Postgres-path loss 1% with Postgres and the load sharing CPUs 4-7

Wild results: #35 p95 65 ms, C# #1 322 ms, v3 540 ms, every v3 dispatch variant 550-960 ms (several failing). v3
showed extra client-bound packets (1.26/req, delayed-ACK timer fires 121/kreq vs 8) and ~80% more Postgres CPU per
request. Rerun with Postgres on its own CPUs (cal7, `screen3.sh`: server 0-1, Postgres 2-4, load 5-7), the same #35
vs v3 pair is equal (p95 28.4 vs 27.0 ms, p99 66.1 vs 61.1 ms): cal4 measured a starvation feedback loop on the rig
(slow answers -> delayed-ACK timer packets -> more load/softirq CPU on Postgres's cores -> slower answers), not the
servers. Stopped after r1. **Rule for the rig from here: Postgres never shares CPUs with the load.**

## Calibration 7 (runs 1-7): Postgres-path loss 1%, Postgres on its own CPUs (screen3: server 0-1, PG 2-4, load 5-7)

30k users ts 0.25, 100k idle, ~11.4k req/s, 2 reps (p99 / p95 ms; worst 5 s p99 in brackets):

| run | r1 | r2 | Postgres sends/req | verdict |
|---|---|---|---|---|
| #35 | 66.1 / 28.4 (105) | 62.9 / 27.0 (206) | 0.10 | baseline |
| v3 (default: fill to 128) | 61.1 / 27.0 (212) | 64.8 / 27.0 (83) | 0.10 | keep (= #35) |
| v3 QUICKACK_OFF=0 | 67.5 / 28.4 | 79.1 / 29.0 | 0.10 | no gain |
| v3 QUICKACK_OFF=0 THIN_LTO=0 | 57.0 / 25.7 | 64.2 / 27.3 | 0.10 | noise |
| v3 PG_RUN=16 | **211.9** / 17.6 | **211.9** / 21.3 | 0.20 | discard |
| v3 PG_DEPTH=0 | 222.7 / 62.3 | 197.6 / 82.3 | 1.0 | discard |
| C# #1 | **209.8** / 12.2 | | 0.26 | reference |

The head-of-line theory was wrong for p99: **deep pipelines recover a lost Postgres segment by fast retransmit**
(many segments in flight -> dup ACKs -> ~1 RTT), short flights (runs of 16, C# #1's scheme) wait for TLP/RTO
(~200 ms, single-packet flights get the delayed-ACK allowance). Our default already has the best Postgres-loss tail;
shallow dispatch only buys p95. PG_RUN stays out.

## The 60-second timeouts: a re-arm theory

mload's per-request timeout is 60 s (k6's default), so #35's grader "timeouts" (holds at 1.48M / 1.41M failed, p99
8.8 s / 1.7 s, CPU ~50%) are requests that hung, not TCP retransmissions. In buffer-ring mode #35 re-armed at most
32 multishot receives that ended in ENOBUFS per loop pass (`budget = min rearm.Count (max back 32)`, `back = 32`
with a ring): a burst that empties the 8,192 buffers (a GC pause, a long pass, softirq starving the thread on 4
cores) leaves thousands of connections unarmed, drained at ~32 per pass (<= 4 ms): seconds of hang, and re-armed
sockets with queued data drain the buffers again. v1+ re-arms every one after the buffers are back (REARM_ALL=1).
Test (cal6): 1M connections, 0.5% loss everywhere, MTU 1500, ~25k req/s, NBUFS=64 to force ENOBUFS, REARM_ALL=0
vs 1; v4 logs max pass time, max CQEs per pass, GC pause total and the re-arm queue.

## Calibration 6 (runs 8-11): 1M connections, 0.5% loss on all loopback traffic, MTU 1500, ~25k req/s, 5-min hold

`scale2.sh`: server 4 vCPU, Postgres 4-6, hold 7.

| run | p50 / p95 / p99 / p99.9 ms | worst 10 s p99 | notes |
|---|---|---|---|
| v4 NBUFS=64 REARM_ALL=0 (#35's re-arm budget, buffers scarce) | 27 / **2120** / 3573 / 3573 | 4252 | stall at t=370: io-0 12,977 queries in flight, 131 ms pass, +117 ms GC, ctxs 19.5k/thread (RSS 1.47 GB) |
| v4 NBUFS=64 REARM_ALL=1 | 27 / **313** / 1258 / 1782 | 2120 | same stall at t=370-390, recovers faster |
| v4 (NBUFS 8192) | 27 / 888 / 2120 / 3003 | 3003 | stall at t=490-510, nobufs=0, ~6.5k in flight on every thread |
| C# #1 | 6 / 1782 / 3573 / 4252 | 5060 | stall at t=410 too |

Every run has a global Postgres stall (all threads' in-flight queries climb together; no checkpoint in the log):
Postgres on 3 rig vCPUs at ~25k req/s runs near capacity, and any blip queues for seconds. A rig limit (the grader's
database box has 16 cores), so absolute numbers mean little; but with the same blip, the old re-arm budget turned a
scarce-buffer stall into p95 2.1 s where REARM_ALL=1 gave 0.3 s, and C# #1 rode its stall worse than v4. No request
went unanswered (timeouts 0) in any run. Next: stall bursts without Postgres saturation (cal9: K=60, SIGSTOP 1.5 s
every 60 s), and CQ overflow drops now logged (`cqdrop`, v5).

## Calibration 8 (runs 12-15): client-path loss 2% each way, Postgres clean (screen3, 30k users)

| run | p99 r1 / r2 | p95 r1 / r2 | worst 5 s p99 |
|---|---|---|---|
| v4 (QUICKACK=0, thin LTO) | 231.7 / 229.4 | 67.5 / 51.1 | 239 / 272 |
| v4 THIN_LTO=0 | 231.7 / 229.4 | 63.6 / 46.2 | 356 / 232 |
| v4 QUICKACK_OFF=0 | 229.4 / 231.7 | 53.7 / 62.3 | 236 / 239 |
| #35 | 229.4 / 231.7 | 41.0 / 49.6 | 232 / 239 |
| C# #1 | 214.0 | 25.2 | 218 |

Client-path loss puts everyone at p99 = one RTO + the server's own latency (C# answers in ~12 ms, we batch to ~27
ms at this per-thread rate; at the grader's ~15k req/s per thread the 128-completion wait fills in ~3 ms). Listener
options are neutral here (2% loss rarely loses the same request twice); kept, as in Go #65, for the multi-loss tail.

## Calibration 9 (runs 16-19): stall bursts, 1M connections, ~16.7k req/s, server SIGSTOPped 1.5 s every 60 s

`scale3.sh` (no loss). Each stall shows as one 10 s window with p99 ~1.5-1.8 s (the stall itself) and recovers in
the next window, for everyone: v5 REARM_ALL=0 p99 1057 ms, v5 1057, #35 1258, C# #1 1057 (worst window 1497 vs our
1782). 0 timeouts, 0 failures. Bursts reached 23k CQEs in one pass (CQ is 32k) but no ENOBUFS: 1.5 s at ~4.2k req/s
per thread queues ~6.3k requests, under the 8,192 buffers. cal10 repeats with 3 s stalls (~12.5k per thread).

## 7 Nov ~16:00 UTC: Denis's direction, and the shipped defaults (v6)

Denis stopped the stall/loss calibration (cal10 killed mid-run: with 3 s stalls both re-arm modes saw ~4k ENOBUFS per
thread and p95 ~1.1-1.3 s, p99 3.0 s; cal11 never ran): the rig isn't the grader, he wants improvements over more
measuring. Directive: spread queries over more Postgres connections with a shallower pipeline per connection (C# #1's
idea), conformance + a quick sanity check, ship the one PR, keep improving on that branch.

Chosen from what was measured: 192 read + 64 write connections (256 <= 512; 4 cores: 48 + 16 per thread) and
`PG_DEPTH=32` fill, not runs of 16 (`PG_RUN=16`, the literal C# scheme, gave p99 ~210 ms vs ~63 ms under 1% Postgres
loss in cal7). Depth 32 caps one loss's blast radius at a quarter of #35's and keeps busy connections' flights
long (~30 segments per 32 feeds); untested under loss. `PG_SPREAD` removed.

Checks on v6: `test/run.sh` 42/42 with THREADS=4, THREADS=8, THREADS=4 LEGACY_BUFS=1, THREADS=8 GZIP_IMPL=fsharp.
Sanity: 1 vCPU 20k users: 68.9 kcycles/req (#35 64.9-71.1 on the same screen), Postgres 222 kcycles/req (#35
182-194: more round trips), p95 15.5 / p99 16.8 ms. 2 vCPU 30k users (screen3): p95 17.1 / p99 42.3 ms, 0 errors,
4.197 segments/req, 128 Postgres connections per thread up, nobufs 0, cqdrop 0.

## v7: Postgres watchdog + thin timeouts on Postgres sockets (pushed to PR #103)

A Postgres connection that owes answers and has received nothing for `PG_TIMEOUT_MS` is dropped (queries fail with
08006 -> 500, connection reopened in the background); idle threads wake once a second so the check still runs.
`TCP_THIN_LINEAR_TIMEOUTS` on Postgres sockets by default (`PG_THIN=0` off). Functional check (wd-v7, 2 vCPU, 30k
users, all Postgres traffic cut for 7 s at ~t=75 s, watchdog at 5 s): connections dropped at 5.2 s ("150 queries
owed"), reconnected with no failure, next window normal; but the cut's requests became 500s (2.6% of the run). So
the default is C# #1's 10 s: frees a stuck connection, while a few-second Postgres hiccup only slows requests.
Checks: 42/42 THREADS=4, THREADS=8, THREADS=4 LEGACY_BUFS=1 (5 s build) and THREADS=4/8 (10 s build). Sanity
(san-v7-2c): p95 17.8 / p99 53.7 ms, 0 errors, 4.195 segments/req, 82.8 kcycles/req (v6 80.8), no watchdog trips.

## v8: 16,384 receive buffers per thread (pushed to PR #103)

`NBUFS` default 8192 -> 16384 (32 MB per thread): in cal10's 3 s stalls (~12.5k requests queued per thread) the 8,192
ran out (~4k ENOBUFS per thread); on the grader's 4 cores the threads share their cores with the kernel's network
work, so late passes are likely. Checks: 42/42 THREADS=4, THREADS=16 (16-core layout: 12 + 4 Postgres connections
per thread), THREADS=4 LEGACY_BUFS=1. Sanity (san-v8-2c): p95 16.3 / p99 42.3 ms, 0 errors, 81.7 kcycles/req, RSS
159 MB (v7 ~120 MB).
