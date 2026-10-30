# Worklog (fsharp-aspnetcore-delneg)

All numbers are **local, NOT official**: a Synteq VPS (8 shared vCPUs, Xeon Platinum 8260, 16 GB, Ubuntu 24.04,
kernel 6.8.0-147) with Postgres 18 on the same box (challenge `postgresql.conf` scaled to 16 GB) and the grader's
sysctls. Server on CPUs 0-3, Postgres + mload + idle holder on 4-7 (`scripts/screen.sh`).

## v1 (30 Oct): first correct version

- `src/Program.fs` 215 lines. `test/run.sh` 42/42 on port 3000 and with the ports swapped (`scripts/conformance.sh`).
- Manual: gzipped `/feed` (2,303 B) unzips to the same bytes as the plain one; `/health` 503
  `{"status":"degraded","db":"unreachable",...}` with Postgres stopped, 200 again ~2 s after it starts.

## Protocol

- **screen** (`scripts/screen.sh`): fresh `feed` from `feed_seed`, Postgres restarted with cold caches; the server as
  a transient systemd unit (user challenge, LimitNOFILE 8388608, CAP_NET_BIND_SERVICE) on ports 80 + 81, CPUs 0-3;
  Postgres, mload and the idle holder (`scripts/idle.c`) on CPUs 4-7. The holder opens 100,000 idle connections
  over 127.0.0.2-9 x both ports, then mload runs 20,000 users at think-scale 0.25 (~8,450 req/s; ramp 20 s, hold
  100 s, `-gzip`). Window +40..+110 s (the idle connections stay under Kestrel's 130 s keep-alive). One number
  decides: server CPU per request (busy time of CPUs 0-3 incl. softirq), plus `perf stat` instructions and cycles.
- **idlecpu** (`scripts/idlecpu.sh`): only N idle connections, cgroup CPU over 30 s, RSS.
- Runs go through `scripts/queue.sh` on the box, so they never overlap.

## Bar: the C# example (examples/csharp, retargeted to net11.0 so the runtime is the same)

| run | us/req | kcycles/req | util | p95 / p99 | RSS at 120k conns | Postgres us/req |
|---|---|---|---|---|---|---|
| C# r1 (old 150 s window) | 301.2 | 749 | 71% | 111 / 352 ms | 2.24 GB | 284 |
| v1 r1 (old window) | 300.5 | 741 | 71% | 27 / 128 ms | 2.24 GB | 179 |

Idle (no load): v1 100k conns 1.65 GB RSS (~15 KB/conn), 0.093 cores; 200k 3.15 GB, 0.168 cores (Kestrel's 1 s
heartbeat walks every connection). C# 100k: 1.77 GB, 0.093 cores. Same Kestrel, same cost.

**Profile of v1** (perf, 20 s, 4 cores): 18% at one libcoreclr address in thread-pool workers (spin-waiting),
memset in zlib's `DeflateInit2` 3.8% (a fresh deflate state per gzipped response), Kestrel heartbeat 3.6%,
kernel 26%.

## Experiments

Format: what, A/B numbers (interleaved), line cost, verdict. LOC = `wc -l src/Program.fs`.

- **e1 `DOTNET_ThreadPool_UnfairSemaphoreSpinLimit=0`** (default 1024 in .NET 11; idle workers spin before
  sleeping). base 310.5 / 311.0 us/req, 774 / 771 kcycles; spin 0: 280.9 / 278.3 us/req, 648 / 644 kcycles
  (-10% time, -16% cycles), p95 19-23 ms both, p99 79-110 both. +2 lines in `start.sh`. **Kept.** LOC 215.
