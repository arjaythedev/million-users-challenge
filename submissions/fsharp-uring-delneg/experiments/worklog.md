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
