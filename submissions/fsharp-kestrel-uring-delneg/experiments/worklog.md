# Worklog (F# million users, delneg)

All numbers are local, on a Synteq box (8 shared vCPUs, Xeon Platinum 8260, 16 GB, Ubuntu 24.04, kernel
6.8.0-147), not the grader's box. Postgres 18 on the same box with the challenge's `box/postgresql.conf` scaled to
16 GB (shared_buffers 2 GB, max_wal_size 2 GB for the small disk; jit off, memoize off, max_connections 600), the
grader's sysctls from `box/setup-app-box.sh`.

## Phase 0 (28 Oct)

- Upstream changed the scoring on 28 Oct: three scores (16, 8, 4 cores), 16 capped at 1,000,000, 8 and 4 searched
  to 3,000,000; LimitNOFILE 8,388,608. #1 rust-tokio-cknutson12 is merged and scored 1,000,000 (max) / 1,210,000 /
  1,130,000; its failing probes (1.24M on 8, 1.16M on 4) failed on latency with app CPU 48% / 79%.
  On 4 cores at 1.13M: 79% of 4 cores for 35.9k req/s = ~88 us of CPU per request, kernel included. The bar.
- Box: feed_seed built by seed/make-seed.sh as postgres, then the tables handed to role `feed` (the tests run as
  `feed`). Reference #1 built from upstream main: 42/42.
- v1: 42/42 on the buffer ring (registered on the `resv[0] = 1` retry: -147 has the inverted check) and with
  `LEGACY_BUFS=1`. Manual: gzip body identical to the plain one (1,901 B), HTTP/1.0 keep-alive, `/health` 503 while
  Postgres is stopped and 200 again ~3 s after it starts.

## Protocol (28 Oct)

- **screen** (`scripts/screen.sh`): fresh `feed` from `feed_seed`, Postgres restarted with cold caches; the server
  as a transient systemd unit (user challenge, LimitNOFILE 8388608, CAP_NET_BIND_SERVICE) on ports 80 + 81, pinned
  with `AllowedCPUs`; Postgres, `mload` and the idle holder (`scripts/idle.c`) on the other CPUs. The idle holder
  first opens the idle connections over 127.0.0.2-9 x both ports (one GET /health each), then mload runs
  (ramp 30 s, hold 120 s, `-gzip`). The window is +60..+150 s after mload starts: busy time of the server's CPUs
  from /proc/stat (user + system + irq + softirq), the unit's cgroup CPU, `perf stat` instructions and cycles on
  the server's CPUs (from e2 on), requests from mload's 5-s ticks. One number decides: server CPU per request.
- **Standard screen = 1 server core** (CPU 0, so 1 I/O thread), 20,000 users at think-scale 0.25 (~8,450 req/s,
  about the per-core rate of #1's 1.13M users on 4 cores) + 100,000 idle connections. Postgres costs ~160-270 us
  per request on this box (the feed query), so 4 server cores at 4x the rate would need ~9 cores of Postgres.
- Calibration on 4 server cores (cal1, 8.45k req/s, 50k idle): 85.9 us/req at 19% utilisation (more wake-ups per
  request when idle).

## References and baseline (28 Oct, 1 core)

| run | us/req (CPU 0) | softirq | app cgroup (user / sys) | util | p95 | RSS at 120k conns |
|---|---|---|---|---|---|---|
| v1 r1 | 48.0 | 13.4 | 49.9 (19.7 / 30.2) | 42% | 1.8 ms | 97 MB |
| #1 rust-tokio-cknutson12 r1 | 60.5 | 14.8 | 62.1 (30.4 / 31.7) | 53% | 1.8 ms | 139 MB |
| v1 r2 | 50.9 | 13.3 | 53.1 (22.2 / 30.8) | 45% | 2.0 ms | 97 MB |
| #1 r2 | 69.7 | 14.7 | 71.7 (38.5 / 33.1) | 61% | 2.5 ms | 139 MB |

v1 costs ~20% less CPU per request than the leader at the same load, with less memory.

**Profile of v1** (perf, 20 s, 1 core): kernel 68% (TCP on both ends of loopback, io_uring, apparmor hooks,
conntrack 1.4% from docker's NAT rules on this box), libdeflate ~17.5% (`deflate_compress_fastest` 11.6%,
`deflate_flush_block` 2.4%, `deflate_make_huffman_code` 1.6%, `sort_symbols` 1.2%): ~19 us per gzipped feed.
Managed code is small. So gzip is the biggest user-space cost.

## Experiments

- **e1 wait for 128 completions or 2 ms after a busy pass** (#30; `WAIT_N=128 WAIT_US=2000`, same build, rotated
  x3): base 46.4 / 48.7 / 58.1 us/req vs 53.9 / 43.2 / 48.1 (medians 48.7 vs 48.1); p95 1.4-2.1 -> 8.0-11.0 ms. At
  ~8.5k req/s a pass rarely fills to 128, so every pass waits the full 2 ms. **Discarded** (no gain beyond the
  noise). The noise between identical runs (46-58 us) is mostly user time on this shared-vCPU host: from e2 on the
  screens also count instructions and cycles with `perf stat`.
- **e2 gzip with ISA-L igzip level 0** (`native/gz.c` shim over pinned libdeflate 1.26 and ISA-L 2.31.1; idea from
  #9 / #12). Per request, `perf stat` over 90 s, rotated x3:

  | arm | kinstr user | kinstr total | kcycles total (r1 / r2 / r3) |
  |---|---|---|---|
  | libdeflate L1 (v1) | 88.9 | 148.6-149.2 | 147.0 / 153.7 / 152.2 |
  | igzip L0 | 46.0 | 107.0-107.2 | 126.7 / 128.2 / 131.3 |
  | igzip L1 | 63.5 | 122.7-123.1 | 128.0 / 133.3 / 133.4 |

  igzip L0 halves user instructions and saves ~15% of all cycles per request. The gzipped feed grows 1,901 ->
  2,243 bytes (still 2 TCP segments). **Kept**, now the default (`GZIP_IMPL=libdeflate` brings v1 back).
  `GZIP_TRAIN=1` (retrain igzip's Huffman tables from a live body at most once a second) is in, untested.

- **mem-v1** (4 threads, holder on the same box, so slab counts both ends): RSS 66.9 MB idle, 138.6 MB at 150k,
  143.9 MB at 400k connections; slab 263 MB -> 3,638 MB, ~8.4 KB per connection pair (~4.2 KB per socket). At 3M
  that's ~12.6 GB of kernel memory for the server's sockets alone; the app itself stays flat. Health 200 after.

- **Box change (28 Oct):** docker, docker.socket and containerd stopped and the nft ruleset flushed (saved in
  `/opt/muc/nft-before.txt`): docker's iptables-nft NAT rules put conntrack on every loopback packet, which the
  grader's app box doesn't have. Kernel instructions per request fell from 61.1k to 53.9k for the same build. All
  numbers from here on are on the clean box (new base runs each time).
- **e3 igzip with retrained Huffman tables** (`GZIP_TRAIN=1`, at most once a second from a live body, #9/#12): CPU
  neutral (kinstr 100.0 vs 100.0, kcycles 119.7 / 121.6 / 116.3 vs 120.7 / 118.1 / 117.0), feed 1,961 vs 2,243
  bytes. **Discarded** as a default (no CPU gain); the option stays.
- **e4 = e1 again with instruction counts** (wait after a busy pass for N completions or T): 32 / 1 ms: 90.8k
  instructions per request vs 100.0k (-9%), cycles median 100.6k vs 118.1k (-15%), 0.13 loop passes per request vs
  1.96, p95 3.5 ms vs 1.4-1.8 ms. 128 / 2 ms: the same CPU (90.0k, 101.4k) with p95 8.4 ms. The /proc/stat time
  (tick-sampled) shows no gain for 32 / 1 ms (52-55 us vs 53 us), the app cgroup a small one (40-44 vs 45-46 us).
  Postgres CPU per request rises (147-157 -> 189-217 us): the queries of a burst go out over all 64 connections
  at once, waking many backends (next: fill a few connections first). **Kept, 32 / 1 ms the default**; e1's
  "discard" was the time noise. Hot screens (1 core near saturation) queued to confirm.
- **Overload check** (`scripts/overload.sh`, 4 cores, e2 build): 30k connects closed at once (half by RST), 20k half
  requests, 20k gzip feeds never read (half RST), 2,000 connections with 64 pipelined feeds each and a 4 KB receive
  buffer, closed after 3 s. After: same PID, health 200, gzip feed ok, post 201, 229 fds. Pass.
- **e4 confirmed hot** (36k users at ts 0.25, ~13.8-15.2k req/s on the one core, still only 55-67% busy since
  Postgres and mload share the other 7 vCPUs): 32 / 1 ms vs no wait: 89.7k vs 95.7k instructions (-6%), 94.6-99.6k
  vs 106.6-112.0k cycles (-11%) per request, utilisation 55-59% vs 66-67%. Committed as the default.
- **Profile of e2 on the clean box**: kernel 75% and flat. Much of it is loopback work for the peers (TCP receive
  of mload's and Postgres's sockets, `ep_poll_callback` waking mload) that the grader's remote clients and database
  do on their own machines, so local per-request numbers overstate the kernel share. igzip ~2.7%.
- **e5 no gzip** (compression is optional, SPEC L25; #31 skips it): -17% instructions and -6% cycles per request
  (1 core, 20k users), -8% cycles hot. **Not adopted**: SPEC adds "the box's network is part of the test: the load
  is measured with compressed responses in mind", and the grader's NIC speed is unknown (a Synteq VPS like this one,
  where ethtool says "Unknown"). Uncompressed, `/feed` (46% of requests) is ~5.3 KB with headers instead of ~2.4 KB:
  at 1.2M users ~0.95 Gbps out. Worth revisiting only if the NIC is known to be >= 10 Gbps.
- Harness: the first `test/run.sh` right after a screen failed to bind 127.0.0.1:3000 (errno 98): the screen's
  client sockets had used 3000 as an ephemeral source port. Reserved 3000, 3001 and 3100 on the box
  (`ip_local_reserved_ports`, test box only). The server's own listeners set SO_REUSEADDR + SO_REUSEPORT, so a
  restart over its own TIME_WAITs binds fine.
- **Upstream PR #35 opened** (28 Oct, after v1 + e2 + e4): https://github.com/arjaythedev/million-users-challenge/pull/35,
  body in `/tmp/muc/pr-body.md` on the Mac (local numbers marked not official). No CI checks reported yet (fork PR).
- **e6 fill the Postgres connections in order** (`PG_DEPTH=n`: a query goes to the first open connection of its
  kind with fewer than n queries in flight; the least loaded when all are that deep). After e4, the queries of a
  wake-up went out over all 64 read connections, one each. Filled, they share a few connections and a few sends,
  and Postgres wakes a few backends. Depth 8 vs least-in-flight: 75.4k vs 90.8k instructions (-17%), 80.3-85.6k vs
  103.9-105.6k cycles (-23%) per request, Postgres CPU 131-136 vs 180-215 us per request (-33%), completions per
  request 2.76 vs 4.06; p95 5-6 ms vs 3.5-3.9, p99 23-29 ms vs 6. Depth 4 is halfway. **Kept**; depths 16 and 32,
  a simulated remote Postgres (netem 0.5 ms) and hot screens are queued to pick the default.
- **e6b picks the depth.** One core, 20k users: depth 8 75.5k instructions / 80-84k cycles per request, p99
  28-32 ms; 16: 73.5k / 77-85k, p99 52-64 ms; 32: 72k / 79-80k, p99 111-123 ms; least-in-flight 90.8k / 100.5k.
  With 0.5 ms netem delay on everything sent to port 5432 (a remote Postgres): 0: 90.9k / 99-104k, 8: 77.9k /
  84.5-86k (p99 30 ms), 16: 75.5k / 83-90k (p99 53-62 ms): the pipeline hides the round trip. Hot (36k users,
  ~15.2k req/s): 0: 89.8k / 95.2k at 63% busy, Postgres 195 us/req; 8: 73.8k / 70-75k at 48-50%, Postgres
  115-129 us/req; 16: 71.8k / 67.4k at 46%, p99 70 ms. **Depth 8 is the default**: 16 and 32 save a few percent
  more for two to four times the p99. 42/42 in both buffer modes, overload check passed.
- **#1 rust-tokio-cknutson12 again with counters** (clean box, same load): 167.7k instructions and 170-174k cycles
  per request; hot 161-162k / 154-156k at 90% busy. This build: 75.5k / 80-84k, hot 73.8k / 70-75k: about half.
- **e7 runtime knobs**: `DOTNET_TieredPGO=0` +3.2k instructions per request (78.5k vs 75.3k), cycles within noise;
  `DOTNET_TieredCompilation=0` +15k instructions and +18k cycles. **Discarded**; R2R + tiered PGO stay.
  (`start.sh` now keeps a `DOTNET_TieredPGO` given in the environment, for such screens.)
- **Profile of e6 with managed symbols** (`DOTNET_EnableWriteXorExecute=0` + perf map in PERF mode): kernel 70%
  and flat; igzip ~6.2%; JSON ~5.4% (`putString` 1.7, `putTime` 1.6, `putPost` 1.4, `putInt` 0.8); `PgRow.Set`
  1.0%. Next: e8, JSON written into one reserved span per post, date math once per response day.
- **e8 JSON into one span per post**: `putPost` reserves `6 x (body + author) + 160` bytes once and writes every
  field at a position (no virtual `Reserve`/`Advance` per field), and `putTime` runs the calendar math once per
  response day (a memo in the response buffer object; a pure function's result, not data). 71.5-71.7k vs 75.4k
  instructions per request (-5%), user cycles -5%, total cycles within noise. `scripts/jsondiff.sh` (new) runs two
  builds on one fresh database with odd posts added (quotes, backslash, control characters, DEL, emoji, RTL mark,
  U+2028/2029, 1969 and 2101 timestamps, a 500-character body) and compares `/feed`, gzipped `/feed` and 300+
  `/posts/:id` answers: byte-identical. 42/42. **Kept.**
- **A million idle connections** (e6 build, 4 threads, the holder on the same box, so both ends): 1,000,000
  opened and answered, no failure, health 200 after. Server RSS 67 MB empty, 138 MB at 250k, 148 MB at 650k-1M
  (flat). Slab 217 MB -> 8.5 GB for 2,000,205 sockets, ~4.2 KB each; 8.7 GB still available on the 16 GB box. On
  the grader (clients remote, 32 GB) a million costs the server ~4.2 GB of kernel memory and ~150 MB of its own.
- **First 4-core screens** (server on CPUs 0-3, everything else on 4-7; 50k users at ts 0.25, 200k idle): neither
  server got the ~21k req/s asked (e6 build 15.1k twice, p95 1.2-1.3 s; #1 14.7k and 18.6k, p95 6.3 s and 0.24 s)
  while their own CPUs were 25-41% busy: Postgres (~150 us of CPU per request) plus mload and the holder saturate
  the other 4 vCPUs. Invalid as a ranking; per request ours is ~81k instructions vs ~179k. This box can't load a
  4-core server to its limit, so 1-core screens stay the measure.
- e9's first build failed (two more uses of the old buffer variable at Server.fs 315 and 323); the queue went on
  with the previous binary, so its diff and A/A screens were void and were cut. Queues now stop on a failed build.
- **Postgres counters in every screen** from here: a second `perf stat -a -G system.slice/system-postgresql.slice`
  over the same 90 s gives Postgres's instructions and cycles per request, steady to ~1% where its CPU time swung
  99-215 us between identical runs. Postgres does ~340k instructions per request, ~4.5x the server.
- **e10 batched Sync for reads** (`PG_BATCH_SYNC=1`, #31's idea): on read connections, the reads a pass sends share
  one Sync (one implicit transaction in Postgres instead of one each); each read is answered at its
  CommandComplete; after an error Postgres skips the rest of the batch up to the Sync, so those are resent. Writes
  keep a Sync each. Server 69.6k vs 71.4k instructions (-2.5%); Postgres 328k vs 344k instructions (-4%) and
  287k vs 312k cycles (-8%) per request. jsondiff identical; 42/42 both ways. The first error-path run was void:
  `statement_timeout=1` also cancelled the statement preparation at connect, and the e9 screen orphaned by
  killing its queue stopped the server mid-run (lesson: kill a queue's children too). Redone with
  `pg_cancel_backend` on our active backends every 50 ms under load (queue 16).
- **e9 response buffer as a sealed class**: `Json.Out` is now a sealed class with plain fields; it was an object
  expression over `let mutable` locals, which F# turns into heap ref cells behind virtual calls. User instructions
  35.9k vs 37.0k, user cycles -8%, total within noise. jsondiff identical, 42/42 both buffer modes. **Kept.**
- **e10 error path, redone**: standard load with batched reads while `pg_cancel_backend` hit our active backends
  every 50 ms for 60 s: 1,451 calls cancelled 109 statements, and exactly 109 requests got a 500 out of 761k (one
  per cancelled statement; the rest of each failed batch was resent and answered), no exceptions, in-flight back
  to 0, p99 13.6 ms, 42/42 after. **e10 kept, batched reads on by default** (`PG_BATCH_SYNC=0` turns them off).
- **e11 depth 16 with batched reads**: Postgres 259-270k cycles per request vs 304-316k at depth 8 (-14%;
  instructions -0.6%: the same work with fewer wake-ups), server -2% instructions, -5 to -11% cycles; p99 65-76 ms
  vs 19-33 ms, p95 ~6 ms vs ~5 ms. #1 (rust) on the same load makes Postgres spend 362k instructions and 453-481k
  cycles per request: with depth 16 and batched reads Postgres spends 10% fewer instructions and ~44% fewer cycles
  per request on us. #1 scores 1.13M users on 4 cores but only 1.21M on 8, which looks like a Postgres ceiling, so
  Postgres cycles likely decide the 8-core score. **Kept: depth 16 is the default**; p99 stays far under the 1 s
  limit.
- e9 + e10 together (batched reads on by default): jsondiff identical to e8; 42/42 default, `PG_BATCH_SYNC=0` and
  `LEGACY_BUFS=1`; overload check passed. Now the box's current build.
- **e12 threads per core** (2 server vCPUs, ~11.5k req/s, 150k idle): 1 thread 66.4k instructions / 72-77k
  cycles per request, 2 threads (default) 68.4-69.5k / 86-88k, 4 threads 72.6k / 98-99k; Postgres cycles follow
  (247-290k, 288-303k, 328-333k). At partial load fewer threads wake up less often and batch more, but a thread
  can use one core, and the score is set at saturation. One thread per core stays. (A possible later idea:
  fewer threads than cores while load is light, i.e. adaptive, not worth the complexity now.)
- **e13 group commit** (#23's idea): the writes a pass sends on a write connection share one Sync, so Postgres
  commits them together; each is answered after the commit; if one fails (23503 on a like), Postgres rolls back
  the ones before it and skips the rest: the failed one gets its 404, the others are reset (`PgQuery.Retry`) and
  sent again. Postgres 321-322k vs 325-326k instructions (-1.3%), cycles median 240k vs 263k (-9%); server the
  same; p99 5.5-34 ms vs 45-61 ms, p95 4.2-5.4 vs 5.6-6.8 ms: at depth 16 the writes had queued behind each other's
  commit on one connection. `scripts/grouptest.py` (300 requests at once over 300 connections, one thread: 150
  creates, 75 likes, 75 likes on a missing post, shuffled, so nearly every write group fails and is resent): every
  create 201 and in the database once with the id it was answered with, every missing like 404. **Kept, on by
  default** (`PG_GROUP_WRITES=0` turns it off).
- e13 checks with the test fixed (a unique tag per round): 9 group-test rounds (grouped x2, off) and 3 on the new
  default build, all OK (repeat likes 200 from round 2 on, as they should); 42/42 default, `PG_GROUP_WRITES=0` and
  `LEGACY_BUFS=1`; overload check passed. Now the box's current build.
- **e14 longer coalescing waits** (2 server vCPUs, 2 threads, ~5.7k req/s per thread): 128 / 4 ms vs 32 / 1 ms:
  server 74.5-74.8k vs 90.3-96.5k cycles per request (-20%), instructions -5%; Postgres 264-275k vs 309-318k
  cycles (-14%); p95 16-18 ms vs 26-30 ms (better, not worse), p99 33-55 vs 46-75 ms. 64 / 2 ms in between. At
  this per-thread rate the 1 ms timeout ends most waits long before 32 completions, so a pass carries few
  requests. Confirming on the 1-core screens (normal and hot) before changing the default.
- **e14 on 1 core**: 128 / 4 ms vs 32 / 1 ms: 5.4% fewer instructions, 12% fewer cycles per request (63.2k / 64.3k
  vs 66.8k / 73.3k); Postgres about the same; p95 15-16 ms vs 4.4-5.2, p99 16-19 vs 10-13. Hot: -3% instructions,
  -5% cycles, p95 13.6 vs 4.5-7 ms, p99 17 vs 25-57 ms. **Kept: 128 / 4 ms is the default**; the gain is largest
  where each thread is least busy (2 threads at 5.7k req/s each: -20% server and -14% Postgres cycles), as on the
  8- and 16-core runs.
- **e15 256 / 8 ms** (2 vCPUs, 2 threads) vs 128 / 4 ms: ~7% fewer server cycles and ~4% fewer Postgres cycles,
  instructions the same, p95 29 ms vs 17-18 ms. **Discarded**: diminishing returns; 128 / 4 ms stays.
- Note: commit 8998b43 (README results) also carried e17's `PIN_THREADS` option (off by default) before its
  screens: a `git add -A` slip. Its build passed and 42/42 with `PIN_THREADS=1` on the box right after; the
  default path only adds one `sched_getaffinity` call at startup.
- **e17 thread pinning** (`PIN_THREADS=1`, 2 vCPUs): 70.9-81.0k vs 69.1-74.9k cycles per request, Postgres the
  same: no gain here. **Discarded** (the option stays, off). On the grader RPS spreads receive work over all cores,
  which loopback doesn't show; untested there.
- **Leaderboard, 28 Oct evening**: #1 is now java-epoll-stohirov (PR #2): 1,000,000 (max) / 1,310,000 / 1,260,000;
  #1 rust 1,000,000 (max) / 1,210,000 / 1,130,000. Both gain only 4-7% from 4 to 8 cores: a shared ceiling near
  1.2-1.3M users (~50-55k req/s), most likely the one Postgres box (the maintainer says so for 16 cores). If so,
  Postgres cycles per request decide the 8-core score and maybe the 4-core one too: ours is ~245k on one thread vs
  ~465k for #1 rust. Their README: own DEFLATE (10 us per feed), group commit while the writer is busy, a 32 / 2 ms
  wake-up batch, TCP_DEFER_ACCEPT (rejected here before: the client sends at once), reads not batched.
  #4 (zig) failed on the grader with the 6.8 PBUF_RING EINVAL, which this entry handles (retry, then legacy).
- **e16 depth 32** (now that group commit keeps writes from queueing): one core, Postgres 218-222k vs 236-247k
  cycles per request (-9%), hot 233-237k vs 261-264k (-10%), with 0.5 ms netem 218k vs 243k; server -1 to -2%
  instructions; p95 and p99 the same (15 / 16.5 ms). **Kept: depth 32 is the default.**
- **e18 Postgres cost vs threads** (current defaults, 2 vCPUs): Postgres 248k cycles per request with 1 thread,
  252k with 2, 281-289k with 4 (+13-16%; instructions +4%). A 4x longer window at 4 threads (512 / 16 ms) wins
  back only 3% for p95 52 ms. So it's the number of concurrently busy backends, not the batch size. At the grader's
  rates Postgres needs many busy backends anyway, so no cross-thread funnel. No change.
- **Saturation, one server core** (e16 build): 50k users -> 19.1k req/s at 43% busy; 60k -> 25.3k at 51%, p95 36
  ms; 70k -> 29.4k req/s at 65% by the end of the run (p95 71 ms, p99 102 ms then), the run's p99 3.6 s coming
  from the ramp while Postgres (~2.8 vCPUs) and mload (1.7 GB) saturate the rest of the box. #1 rust at 40k users:
  16.9k req/s at 99% busy, p95 194 ms, p99 562 ms. At ~57-59k cycles per request one core of this server would
  carry ~40k req/s; the grader's Postgres box will be the limit long before the server.
- **e16 committed** after the rebuild: 42/42 both buffer modes, group test 3/3, overload check passed.
- **r1 Postgres restart under load** (1 core, standard 20k-user screen, `systemctl restart postgresql` 70 s in):
  same PID, health 200 at 3 s and 8 s, tests 42/42 after, but 87 `InvalidOperationException: EmptyQueue` in
  `PgConn.Answered`. Cause: on shutdown every backend sends an ErrorResponse with severity FATAL (57P01). The
  batched and group-commit handlers took it as the answer to the oldest query, whose Sync might not even be sent
  yet (`syncs` empty). **Fix**: a FATAL/PANIC ErrorResponse, or a C/E/Z with nothing sent for it, now fails the
  connection (its queries get 500s, the slot reconnects); `Received` stops at a broken connection and the loop
  hands it to `PgLost`. A first version also called a ReadyForQuery with an empty `inflight` unexpected, which is
  normal after group commit and batched reads have answered everything: it broke the write connections for good
  (2.9% errors, tests failing after); the check for Z now looks at pending Syncs only. Rerun: 0 exceptions, 96
  connections failed with `FATAL 57P01` and came back, 6,428 http 500s (0.56%) during the restart, pass=true,
  tests 42/42; cost the same as e16 (62.7k instructions, 64.0k cycles; Postgres 315k / 220k). **Kept.**
