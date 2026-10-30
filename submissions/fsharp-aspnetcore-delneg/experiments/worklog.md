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
- **e2 `DOTNET_SYSTEM_NET_SOCKETS_INLINE_COMPLETIONS=1`** (before and after e1): 309 / 313 vs 311 us/req;
  on e1: 281 / 297 vs 277-280. No gain. **Discarded.**
- **e3 GC modes** (on e1). DATAS off (`GCDynamicAdaptationMode=0`): 278.7 / 279.2 us/req, p99 519 / 735 ms (vs
  66-137). Workstation GC (`gcServer=0`): 256 / 255 us/req by time but 673 / 667 kcycles (more than base 636-655),
  p95 43 ms (vs 16-22), RSS higher. **Both discarded.**
- **gcdump at 100k idle connections** (`scripts/gcdump.sh`, each opened with one gzipped GET /feed): GC heap 879 MB
  = ~8.8 KB per connection, ~100 objects each; RSS 1.50 GB (~15 KB). Top: runtime-async `Continuation` 101 MB
  (~6 per connection), pipe segment pools (`Segment<BufferSegment>` + `BufferSegment[]` + `BufferSegment`, 2 pipes)
  124 MB, `Http1Connection` 65 MB, `Pipe` 43 MB, request/response header collections 70 MB, `Lock` 28 MB,
  `SocketConnection` 27 MB. All Kestrel/BCL internals: no app code holds anything per connection.
- **e4 gzip only bodies over 1 KB, done in `send`** instead of the ResponseCompression middleware (it set up a
  deflate stream, whose init memsets the state, for every 30-byte like reply), compressed into memory so every
  response has a Content-Length (no chunked encoding). First try: all 500s under load (`GZipStream` closed the
  MemoryStream; the test suite sends no `Accept-Encoding`, so `scripts/conformance.sh` now also checks gzipped
  bodies unzip to the plain ones). Fixed (`leaveOpen = true`). base 280.6 / 283.2 / 285.8 / 280.9 us/req, 646 /
  660 / 674 / 650 kcycles; gzip-in-send 263.6 / 272.1 us/req, 573 / 607 kcycles: **-5% time, -10% cycles**, p95/p99
  unchanged. +13 lines. **Kept.** LOC 228.
- **e5 Npgsql `Multiplexing = true`** (on e1, pool 32): base 275.3 / 275.6 us/req, 627 / 631 kcycles; multiplexing
  275.0 / 271.4 us/req, 610 / 592 kcycles (-0.8% time, -4.5% cycles), Postgres -10% cycles per request, p95 6.3 ms
  vs 10.6 / 22.6. Pool 16 + multiplexing: 279.4 us/req. 1 line. **Kept** (cycles and tail; it pays more as load
  grows, to be confirmed in holds).
