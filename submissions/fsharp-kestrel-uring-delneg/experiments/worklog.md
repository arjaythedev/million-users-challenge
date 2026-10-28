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
