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
- **e6 idle-memory knobs** (`scripts/idlecpu.sh`, 100k idle connections, RSS): base 1.645 / 1.632 GB;
  `GCConserveMemory=7` 1.596; `GCHeapCount=2` 1.645; `DOTNET_RuntimeAsync=0` 1.608; workstation GC 1.451 (but
  see e3: worse tail). Nothing worth it: the memory is Kestrel's live per-connection objects (gcdump above).
  **Discarded.**
- **Simplify pass 1** (`simplify` skill, by hand: single pass): one `stringProp` helper for the JWT claims and the
  post body (was two), `readBody` without the tuple match, the HMAC over the token's prefix (no string concat),
  `JsonElement.Parse` for the JWT parts (no undisposed `JsonDocument`). 230 -> 227 lines. A/B: base 264.3 / 261.4
  us/req, simplified 261.1 / 261.5. 42/42. **Committed.** LOC 227.
- **e7 Kestrel socket options** (a test build reading them from env; on e5): base 264.1 / 273.5 us/req, 555 / 601
  kcycles. `IOQueueCount=1`: 261.6 / 265.0; `=2`: 264.3 / 260.1 (~1%, discarded). `UnsafePreferInlineScheduling`
  alone: 262.9 / 263.0 (none). **Inline scheduling + `DOTNET_SYSTEM_NET_SOCKETS_INLINE_COMPLETIONS=1`** (requests
  run on the epoll threads, no thread-pool hop): 246.3 / 247.0 us/req, 516 / 521 kcycles (**-8% time, -10%
  cycles**), p95 5.7-5.8 ms (vs 7.6-19.6), p99 79-87 ms. 3 lines + 2 in start.sh. **Kept.** LOC 231.
- **Profile of e7** (perf, 4 cores): Kestrel heartbeat ~6.7% (walks every connection once a second), zlib
  `deflateInit` memset ~4% (zlib-ng clears its hash table on every init or reset, so a reused encoder wouldn't
  help), STJ ~1-2%, `DateTime` formatting 0.8%, GC 1.7%. Flat otherwise.
- **e8 knobs on e7.** `DOTNET_GCgen0size=0x10000000`: 246.0 / 233.7 us/req vs base 247.5 / 243.1, p99 494 / 306
  ms vs 78 / 78: worse tail. **Discarded.** `DB_POOL=16`: 248.4 / 242.3; `DB_POOL=64`: 248.2 / 243.9: no change
  with multiplexing. **Discarded** (default stays min(128, 8 x cores)).

## Holds vs the C# example (2 server cores, CPUs 0-1; Postgres + mload + idle holder on 2-7)

Same box, same method: 100k idle connections plus N users at think-scale 0.25, 180 s hold (`HOLD=180s`; the idle
holder pings every 60-100 s so idle connections stay open; the first series used 100 s, which let ~20% of them hit
the 130 s keep-alive for both servers alike). Pass = p95 < 500 ms, p99 < 1 s, errors < 1%.

| users (~req/s) | C# example | e7 |
|---|---|---|
| 12k (~5.1k) | pass, 82% busy, 299 us/req, p95 24 / p99 59 ms | pass, 64% busy, 245 us/req, p95 20 / p99 47 ms |
| 16k (~6.7k) | pass, 87%, 245 us/req, p95 61 / p99 150 ms | pass, 73%, 206 us/req, p95 27 / p99 47 ms |
| 20k (~8.4k) | pass (barely), 95%, 223 us/req, p95 179 / p99 524 ms | pass, 83%, 188 us/req, p95 41 / p99 103 ms |
| 24k (~10k) | **FAIL**, 97%, p95 2.1 s / p99 2.6 s | pass, 90%, 172 us/req, p95 61 / p99 165 ms |
| 28k (~11.8k) | - | pass, 92%, 152 us/req, p95 105 / p99 405 ms |

So on 2 cores, local, NOT official: the C# example holds ~20k of these users, e7 at least 28k (+40%).
- **e9 Kestrel options** (test build reading them from env, on e7): base 247.6 / 245.2 us/req.
  `WaitForDataBeforeAllocatingBuffer=false` 248.9 / 242.9 (and +3.9 KB RSS per idle connection: 1.82 vs 1.43 GB
  at 100k); `AddServerHeader=false` 247.3 / 249.2; `MinResponseDataRate`/`MinRequestBodyDataRate = null` 250.9 /
  249.3. **All discarded.**
- **e11 runtime knobs on e7** (base 240.6 / 243.0 us/req, 498 / 508 kcycles). `GCHeapHardLimitPercent=0x46`:
  240.1 / 248.8 (neutral); `gcConcurrent=0`: 240.9 / 244.9, p99 147 / 198 ms (vs 67-71): worse;
  `GCNoAffinitize=1`: 245.9 / 246.2: worse. `DOTNET_SYSTEM_NET_SOCKETS_THREAD_COUNT=2` (default = cores with inline
  completions): 234.2 / 235.0 us/req, 474 / 473 kcycles (-3% time, -5% cycles), p95/p99 unchanged: pending a
  4-core high-load hold (fewer socket threads could queue at saturation).
- **e10 `GZipEncoder.TryCompress`** (.NET 11's one-call span encoder, quality 1) instead of MemoryStream +
  GZipStream: base 248.0 / 243.5, encoder 242.7 / 250.0 us/req. Neutral, same line count, and the `mutable` +
  `&length` out-parameter reads less plainly. **Discarded.** (.NET 11's `ZLibCompressionOptions` has a window size
  but no memory level; zlib-ng's hash table is cleared on every init either way.)
- **`DOTNET_ReadyToRun=0`** (jit the framework too): 241.7 / 240.8 vs 243-248. Noise. **Discarded.**
- **e12 Oxpecker 2.1.1 routing** (the brief's alternative to minimal APIs; `GET [ route ...; routef
  "/posts/{%s}" ... ]`, handlers take the id string): 42/42; base 242.0 / 241.0, Oxpecker 243.7 / 241.4 us/req.
  Same speed, +3 lines and 4 more packages (Oxpecker, Oxpecker.ViewEngine, TypeShape, RecyclableMemoryStream).
  **Not adopted**: endpoint routing is already in the framework and reads fine with `RequestDelegate` handlers.
- **Holds on 4 cores** (IDLE_K=60, 180 s): e7 at 32k users (13.5k req/s): pass, 75% busy, 204 us/req, p95 47 / p99
  109 ms; at 40k (16.8k req/s): pass, 82% busy, 178 us/req, p95 81 / p99 175 ms. With 2 socket threads: 32k 70%
  busy, 194 us/req, p95 42 / p99 103; 40k 75% busy, 171 us/req, p95 85 / p99 172. Fewer socket threads holds up
  at high load (-7% CPU at 40k).
- **2 socket threads, 2-core hold** (the rule would be "half the cores"): 1 socket thread on 2 cores at 32k users
  FAILS (p95 788 ms, p99 981 ms: one thread saturates) where the default passes (p95 85 / p99 170 ms). So on 4
  cores, 2 threads would cap just above 40k too. **Discarded**: the socket thread count stays = cores.
- **`GCHeapHardLimitPercent=0x46` adopted** as insurance, not for speed: CPU-neutral (e11), and the C# example's
  README measured Server GC heap growth to ~29 GB with collapsing latency at ~800k connections without it, on the
  same Kestrel. 2 lines in `start.sh`.
