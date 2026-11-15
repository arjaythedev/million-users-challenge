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

## DB path checks (Opt's questions, 30 Oct)

- **Prepared statements are used under load** (`scripts/check-prepared.sh`: `log_statement=all` for 10 s under
  2,000 users, current build, multiplexing on): `execute _auto1` 4,023, `_auto0` 3,889, `_auto2` 575, `_auto3` 72,
  `<unnamed>` 5, plain `statement` 2. So 99.9% of the statements run as Npgsql auto-prepared ones; the few unnamed
  ones are the first uses on each connection, before `AutoPrepareMinUsages=2`. Nothing to fix.
- **Parameter types**: `AddWithValue` with an `int64` sends `bigint`, with a `string` sends `text`. These match the
  columns (`int8` ids, `text` body), so Postgres doesn't cast, and the plans use the indexes. Typed
  `NpgsqlParameter<T>` (no boxing) is queued as a measurement (it costs about 0 lines).
- **Feed `like_count` subquery vs "fetch the page first, then counts only for the returned rows"** (Denis's
  screenshot): `EXPLAIN ANALYZE` of the feed shows `Limit -> Nested Loop` walking `posts_created_at_id_idx`, with the
  `like_count` SubPlan at `loops=20` (an index-only scan of `likes_post_id_idx` per returned row). Postgres already
  counts only for the 20 rows on the page, and rule 3 fixes the SQL anyway. Nothing to gain.
- **Reader path**: per row, `GetInt64`/`GetString`/`GetDateTime`/`GetInt32` by ordinal plus one ISO string
  (`DateTime.ToString` with an invariant format). STJ + reader together are ~1-2% of the CPU in the v1 profile, below
  the per-request kernel + Kestrel + zlib cost, so a hand-rolled `Utf8JsonWriter` path is not worth its lines.

## Library check (Denis, 30 Oct: "swap in better libraries if they read better and cost no speed")

| package | latest | targets | verdict |
|---|---|---|---|
| Npgsql.FSharp 8.0.0 | 2024-12 | net8, depends on Npgsql 8.0.6 | works on Npgsql 10.0.3 + net11 (42/42); reads by column name; A/B as **e15** |
| Donald 10.1.0 | 2024-03 | net8 | stale, not tried |
| Dapper.FSharp 4.10.0 | 2026-04 | query builder | generates SQL; rule 3 wants the SPEC SQL verbatim, so not tried |
| SqlHydra.Query 5.1.1 | 2026-10 | codegen | generated code + builder; same SQL problem, not tried |
| FSharp.SystemTextJson 1.4.36 | | netstandard2.0 | plain records already serialize correctly with STJ; nothing to add |
| Thoth.Json | | Newtonsoft/Fable | no |
| Giraffe 8.3.0 | | net9 max | Oxpecker is its successor, measured in e12 (equal speed, +3 lines): not adopted |

## Ideas from EgorBo's blazing-fast-dotnet skill (main 02ed682, 27 Oct 2026)

From the skill: e1 spin limit (`ThreadPool_UnfairSemaphoreSpinLimit=0`), e7 inline completions, the DATAS/
workstation GC/`GCgen0size` knobs (e3/e8, discarded), `TieredPGO`/`ReadyToRun`/`TC_*`/OSR, `HillClimbing_Disable`,
`GCDGen0GrowthPercent`, `GCDTargetTCP`, `TC_CallCountingDelayMs` (the k5/j-* runs below), Native AOT. Not from it:
multiplexing (Npgsql docs), Npgsql write coalescing (k5 `coal`), gzip placement (e4).

Bench note: the screen window starts 40 s after mload starts, so tiering/PGO warm-up is mostly over before it, but
the grader counts the whole run, ramp included: R2R/AOT could matter there even if the window shows nothing.

Ops note: the pool-32 runs of e8 logged millions of exception stack traces through journald into `/var/log/syslog`
(2.3 GB, box disk down to 1.1 GB free). Archived it to `/root/muc-aspnet/syslog-20261030.gz` and truncated it; all
bench units now run with `LogRateLimitIntervalSec=30s LogRateLimitBurst=200`.

## Overnight batch, 30-31 Oct (4-core screens: 20k users, ts 0.25, 100k idle; means of interleaved reps)

us/req is server CPU on CPUs 0-3 incl. softirq; kc = server kcycles per request. All local, NOT official.

- **e13 Npgsql knobs** (knobs3 test build, 6 reps): base 247.8 us/req, 526 kc. Write coalescing threshold 1000 B:
  248.4 / 528 (the first-round -2.6% was noise). `TC_CallCountingDelayMs=0` (2 reps): 248.7. KeepAliveTimeout
  infinite: 7.3% EOF errors, broken. **All discarded.**
- **e14 typed `NpgsqlParameter<T>`** (no boxing; 231 lines either way, longer lines; 3 reps): base 241.0 / 496 kc,
  typed 242.3 / 502 kc. 42/42. **Discarded**: no gain, `AddWithValue` reads better.
- **e15 Npgsql.FSharp 8.0.0** (`Sql.fromDataSource |> Sql.query |> Sql.parameters |> Sql.executeAsync`, columns by
  name; 231 -> 224 lines; 42/42; 6 reps): base 245.9 us/req, 518 kc, 184 user kinstr; Npgsql.FSharp 252.6 us/req
  (+2.7%), 544 kc (+5.2%), 208 user kinstr (+13%). It was slower in all 6 pairs (by-name lookups, a list + closures
  per row). Also it is built against Npgsql 8 (Dec 2024). **Discarded**: it reads a bit better, but it costs
  noticeable speed, so it fails Denis's rule.
- **e16 JIT/PGO** (Opt's list; 4 reps unless noted; base 245.0 us/req, 514 kc):

  | knob | us/req | kc | verdict |
  |---|---|---|---|
  | `TieredPGO=0` | 262.2 (+7.0%) | 581 (+13%) | Dynamic PGO is on by default and worth 7%: keep the default |
  | `ReadyToRun=0` + `TieredPGO=1` | 247.2 (+0.9%) | 520 | noise |
  | `TC_QuickJitForLoops=0` | 248.6 (+1.5%) | 530 | noise/worse |
  | `TC_OnStackReplacement=0` | 243.3 (-0.7%) | 509 | noise |
  | `TieredCompilation=0` (2 reps) | 318.3 (+30%) | 813 (+58%) | much worse (no PGO) |
  | `PublishReadyToRun=true` (own assembly) | 246.6 (+0.7%) | 522 | noise |
  | Native AOT (`PublishAot`) | - | - | builds, then aborts at startup: `FileNotFoundException: Microsoft.AspNetCore` (trimmed away); F# + ASP.NET Core + reflection STJ under AOT is not simple. Not pursued |

  Warm-up: the screen window starts 40 s into the run, and tiering is done well before that. Our own assembly is
  ~230 lines and the framework already ships R2R, so R2R/AOT would change only the first seconds of the ramp.
  **Nothing adopted**: the defaults (tiered + Dynamic PGO + framework R2R) are already the best measured.
- **e17 EgorBo-skill knobs** (4 reps, same base): `HillClimbing_Disable=1` 244.0 / 510 kc; `GCDGen0GrowthPercent=
  0xC8` 244.2 / 514; `GCDTargetTCP=1` 242.8 / 503 (-0.9% / -2.2%, but mixed sign by pair: -4.8, +4.1, -2.9, -5.2).
  **All discarded** (under the noise; later rounds ran ~1.5% lower across the board).
- **Profile** (perf, current build): Kestrel heartbeat ~6.5% (`TickHeartbeat` + `ConnectionManager.Walk`, once a
  second over every connection; Kestrel has no setting for it), zlib-ng `deflateInit` memset 2.6% + deflate 0.8%,
  Server GC 1.8%, STJ 1.1%, `DateTime` formatting 0.85%. Flat: no item a readable change can reach is above ~3%.

## Holds, 31 Oct (HOLD 300 s unless noted, IDLE_K=60, 100k idle; local, NOT official)

2 server cores (CPUs 0-1; Postgres + mload + idle holder on 2-7), both at ~97% busy:

| server | users | req/s | p95 / p99 ms | result |
|---|---|---|---|---|
| C# example | 20k (x2, + 600 s) | 7.8-8.1k | 181-283 / 429-692 | pass |
| C# example | 22k | 8.1k | 504 / 1162 | **FAIL** |
| ours | 32k (x2, + 600 s) | 13.2-13.3k | 108-121 / 328-614 | pass |
| ours | 34k | 13.9k | 162 / 499 | pass |
| ours | 36k | 15.0k | 123 / 286 | pass |

4 server cores (CPUs 0-3):

| server | users | req/s | p95 / p99 ms | server busy | Postgres us/req | result |
|---|---|---|---|---|---|---|
| C# example | 24k | 9.8k | 154 / 356 | 80% | 269 | pass |
| C# example | 28k (x2) | 9.8-10.1k | 551-646 / 672-862 | 81-82% | 263-267 | **FAIL** |
| C# example | 32k | 9.7k | 1139 / 1349 | | | **FAIL** |
| ours | 40k | 16.8k | 65 / 133 | | | pass |
| ours | 44k | 18.3k | 118 / 212 | | | pass |
| ours | 48k | 19.7k | 192 / 332 | 85% | 107 | pass |
| ours | 52k | 20.6k | 345 / 621 | 86% | 106 | pass |

Caveat: on 4 cores the C# example fails with its server only ~81% busy. Its Postgres costs ~265 us per request vs
our ~106, which is ~2.7 of the 4 shared CPUs at 10k req/s. So its 4-core limit here is likely the box's Postgres
CPU, not its server. (Our gap: multiplexing, plus `NoResetOnClose` vs the C# example's per-request open/close.
`check-prepared.sh` on the C# example: it is auto-prepared too (`_auto0`-`_auto3` 99.9%), with no reset statements in the log; it runs the same pool size, without multiplexing and without `NoResetOnClose`. Our v1 (`NoResetOnClose`, no multiplexing) already had 37% less Postgres CPU, and e5 cut another ~10%: together about the 2x seen. Likely, not proven.) With a separate DB box the 4-core gap would be smaller than 52k vs 24k.

200k idle, 4 cores, 180 s: ours at 32k users passes (p95 55 / p99 133 ms, 212 us/req, RSS 3.37 GB); C# at 24k
passes (p95 106 / p99 196, 292 us/req, RSS 3.89 GB).

Memory at scale (`mem.sh`, 4 cores, idle connections opened in steps, then each pinged once):

| server | 150k | 250k | 350k | 400k RSS | per conn @400k | pings ok at 400k |
|---|---|---|---|---|---|---|
| ours (2 runs) | 2.29 / 2.37 GB | 3.83 / 3.89 | 5.34 / 5.34 | 6.13 / 5.97 GB | 15.1 / 14.7 KB | 400,000 / 399,040 |
| C# example (2 runs) | 2.83 / 2.65 | 3.03 (200k) / 4.04 | - / 5.08 | 6.82 / 6.31 GB | 16.8 / 15.5 KB | 277,533 / 365,975 |

## Simplify pass 2 (31 Oct)

`simplify` skill, applied by hand in a single pass. One `withId` helper ("valid post id, or 400 invalid post id")
replaces the repeated 4-line match in `getPost` and `like`, and `routeId` is gone. Auth still runs before the id
check, because `authed` wraps `like`. 42/42 on both port orders, plus the gzip and Postgres-down checks.

- As a plain function (228 lines), 5 interleaved pairs: base 246.4 us/req / 522 kc, simplified 251.2 / 535
  (+1.9% / +2.6%, worse in 4 of 5 pairs). The lambda is a closure per request, and its `task` adds a second
  state machine. **Not kept in that form.**
- As `let inline withId ... ([<InlineIfLambda>] handler ...)` (229 lines with its comment), 4 pairs: base 248.7 /
  532 kc, inline 246.3 / 521 (-1.0% / -2.0%). F# expands the lambda in place, so each request has one state
  machine and no closure. **Kept**: 231 -> 229 lines at equal or better speed. LOC 229.

Also on 31 Oct: 2-core hold at 40k users passes at the edge (15.3k req/s, 99% busy, p95 474 / p99 862 ms), so our
2-core limit on this box is ~40k vs the C# example's ~20k. 4-core hold at 56k FAILS (p95 943 / p99 1417 ms) with
our server only 85% busy at 20.7k req/s: above ~52k the shared CPUs 4-7 (Postgres ~2.2 cores, mload, idle
holder) limit this box, not the server.

## e18-e20 (31 Oct)

- **e18 pool size at saturation** (multiplexing on; default was `min(128, 8 x cores)`). On 4 cores at 52k users
  (300 s holds): default (32) 160.7 / 152.9 / 162.3 / 162.4 us/req (mean 159.6), Postgres 113 us/req, 3 of 4
  passed; `DB_POOL=16` 149.7 / 150.0 / 152.8 (mean 150.8, **-5.5%**), Postgres 102 (-10%), 2 of 3 passed;
  `DB_POOL=64` 154.2 (1 run, passed). Build with `4 x cores` (s4), on 2 cores at 40k users: default (16) 133.9 /
  125.6 (mean 129.8, 1 of 2 passed); 4 per core (8) 118.5 / 121.8 (mean 120.2, **-7.4%**, -8% kcycles, 1 of 2
  passed). At 20k users on 4 cores: 249.0 vs 246.0 (one pair) and e8's 16-vs-32, both neutral. With multiplexing,
  fewer connections carry more commands per round trip. That pays only near saturation, which is where max users
  is decided. 42/42. **Kept**: one character plus a comment line. LOC 230. Risk: this box's Postgres shares CPUs
  with mload, which may favour fewer connections; on 16 cores the default becomes 64.
- **e19 `DOTNET_RuntimeAsync=0`**: 400k idle RSS 6.09 GB vs base 5.73 / 5.90 (+4.6%); screens 260.1 / 251.0 vs
  248.1 / 247.5 us/req (+2.6%). **Discarded.**
- **e20 `DOTNET_GCConserveMemory=9`**: 400k idle RSS 4.98 GB vs base 5.73 / 5.90 (**-14%**, all 400k pings
  answered). Screens, 4 pairs: 246.6 vs 247.2 us/req (neutral), but p99 73 / 456 / 261 / 280 ms vs 80 / 98 / 82 /
  84 (compacting gen2 pauses). 48k hold: 159.3 vs 155.8 us/req (+2.2%), p95/p99 164 / 306 vs 162 / 300 ms, RSS
  under load 2.11 vs 2.38 GB. **Discarded**: worse tails at moderate load, +2% CPU at high load. The 70% heap hard
  limit already makes the GC compact harder when memory runs short. If memory per connection turns out to bind
  max users on the grader, this is the first knob to try.
- **Fresh bar** (interleaved, current build, 4 cores, 20k users): C# example 300.9 / 303.0 us/req, 762 / 767 kc,
  p95 62 / 71 ms, Postgres 292 / 299 us/req; ours 257.0 / 247.8 us/req, 568 / 523 kc, p95 18 / 8 ms, Postgres
  155 / 154. **-16% CPU, -29% cycles, -47% Postgres CPU per request.**
- 2-core 44k hold FAILS (100% busy, p95 1173 ms): our 2-core limit here is ~40k (C# ~20k).
- **e21 pool 2 per core** (`DB_POOL=8` on 4 cores, 52k users, 2 pairs vs the new default 16): 160.6 / 152.0 vs
  152.7 / 163.0 us/req (neutral), but both `DB_POOL=8` runs FAIL (p95 845 / 879 ms, 19.2-19.4k req/s) vs the
  default 1 of 2 passing (p95 381 / 906): eight connections become the bottleneck. **Discarded**: 4 per core stays.

## Final simplify pass (31 Oct)

By hand, single pass, same four angles as the skill (reuse, simplification, efficiency, altitude), on all 230
lines. Considered and left alone:
- `Stopwatch` vs `Environment.TickCount64` for uptime: saves one `open`, reads worse.
- Folding `readBody` into `createPost`: the separate function names the "malformed vs missing body" rule.
- `connectionString`'s URL parsing: Npgsql takes no `postgres://` URL, so it is needed.
- `stringProp` / `positiveId`: each already used twice.
- The `send` gzip branch: measured (e4).

No change; 42/42 on the deployed build (`conf-final`).
LOC 230.

## Final state (31 Oct)

230 lines of F# in `src/Program.fs` (231 at v1 + kept changes - simplify passes), 3 knobs in `start.sh`. Kept:
e1, e4, e5, e7, the heap limit, e18, simplify passes 1-2. 44 experiments after v1 (`autoresearch.jsonl`).
