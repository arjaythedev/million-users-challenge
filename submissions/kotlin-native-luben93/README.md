# Kotlin/Native, no framework: own HTTP/1.1 server on epoll, own Postgres client

| | |
|---|---|
| Language / runtime | Kotlin 2.4.21, Kotlin/Native (LLVM) for linuxX64: a native executable, no JVM at run time |
| HTTP | own HTTP/1.1 engine on raw epoll (`platform.linux`), one SO_REUSEPORT listener per event-loop thread and port |
| Postgres | own wire-protocol 3.0 client (SCRAM-SHA-256, extended protocol, prepared statements, binary results) |
| JSON, JWT | own: JSON written straight from binary rows, HS256 with a pure-Kotlin SHA-256 |
| Build | Gradle 9.8.2 (pinned SHA-256) + Kotlin Multiplatform plugin 2.4.21 on Ubuntu's OpenJDK 21 (apt), build time only |
| Dependencies | none besides the Kotlin stdlib |

Based on the author's `kotlin-native-luben93` entry of the multicore server challenge (same engine, Postgres client,
JSON and JWT code), reworked for a million mostly idle keep-alive connections.

Passes `test/run.sh`: `passed=42 failed=0` (Postgres 18.6, both with Postgres on loopback and behind a 250 µs
network delay).

## Design

- **HTTP**: N event loops (`HTTP_THREADS`, default = CPUs from `sched_getaffinity`). Each has its own epoll set and an
  SO_REUSEPORT listener on both ports. Idle connections hold no buffers; each response is one write of headers +
  body with Content-Length.
- **Postgres**: each request runs exactly one SPEC statement, prepared once per connection, with binary results.
  - Read lanes (`DB_READ_CONNS`) take everything queued and pipeline it: Bind/Execute per request and ONE Sync per
    batch, one round trip per batch. Postgres then sends the batch's replies together (a few full packets) instead of
    flushing one small packet per statement, and each handler resumes as soon as its own rows are in. If a read fails,
    Postgres skips the rest of the batch until the Sync: those reads go out again in a new round trip.
    `DB_READ_SYNC=each` keeps one Sync per statement.
  - Write lanes (`DB_WRITE_CONNS`) group-commit (BEGIN; inserts; COMMIT in one round trip) and answer only after
    COMMIT. If one insert fails (a like on a missing post), the batch rolls back and each insert re-runs on its own.
  - Overload safety: queues are bounded (past the cap, requests get 500 at once instead of piling up); a request
    takes its response buffer only when it renders, not while it waits (~0.5 KB per queued request, not 8 KB);
    queued reads whose client already closed the connection are dropped unrun; lane threads survive any error.
- **gzip**: for responses of at least `GZIP_MIN_BYTES` (1024) bytes when the client sends `Accept-Encoding: gzip` (a
  `;q=0` weight refuses it); in practice the feed. A small-input encoder (`native/fastgz.c`, compiled in by
  Kotlin/Native's clang through cinterop): greedy LZ77 over a 4-byte hash plus the fixed Huffman code, one deflate block,
  CRC-32 by slice-by-8. The idea is not new: it is the design of zlib-ng's `deflate_quick` (its level 1, from Intel's zlib
  fork) and of Willy Tarreau's libslz (HAProxy's default compressor), written independently as a one-shot call for
  inputs of a few KB, with the 4-byte multiplicative hash that LZ4, zstd and zlib-ng use and Intel's slicing-by-8 CRC.
  What it saves is per-call setup: the table is 8 KB of 16-bit positions that is never cleared, because an entry is only
  a hint that is checked against the input before it is used, so a stale entry costs one failed probe and no byte of an
  earlier response can reach a later one. On the 3.7 KB feed (one E-core of an i7-13700H, same payload, one reused
  compressor per library) it takes about 4 us where zlib-ng 2.3.3 level 1 takes 8.7 (its `deflateReset` alone is 3.7),
  zlib 1.3 level 1 14.5 to 16.8 and libdeflate 1.26 level 1 14.1; the output is 876 B against 887 B for zlib-ng level 1
  and 750 B for zlib level 1 (the 15% price of fixed codes), still one TCP segment with the headers. libslz, the nearest
  relative, was not measured. The load-generator feed is the 20 newest posts, so it is this shape. Standard gzip, so any
  client decodes it. `native/fastgz_test.c` round-trips every output through zlib's inflate (11,000 cases: prefixes,
  random data, patterns, mutated feeds, matches up to the 32 KB window, one reused compressor so stale entries are
  exercised, a table overwritten with arbitrary hints; run under ASan and UBSan). A separate review decoded outputs with an
  independent strict RFC 1951 decoder over all 255 match lengths and all 32,768 distances, ran guard-page and worst-case
  size checks and ThreadSanitizer: no wrong byte, and two hardening items (a first-use race on the shared tables, an
  unchecked distance of zero), both fixed here. Bodies over 64 KB, or a destination buffer too small for the
  worst case, fall back to zlib (`platform.zlib`, level `GZIP_LEVEL`, default 1; 0 = no compression);
  `GZIP_IMPL=zlib` forces zlib everywhere.
- **JWT**: HS256 with a pure-Kotlin SHA-256; signature, alg and exp are checked on every request.
- **Stats**: one line every `STATS_INTERVAL_S` (10 s, 0 = off), from per-thread counters and histograms. It shows
  conns, accepts/s, req/s per endpoint, server p50/p99, Postgres p50/p99, jobs per round trip and per commit, queue
  depths, 4xx/5xx/shed, gzip ratio and µs, MB/s out, partial writes and RSS, then blocks for the box and the process:
  `gc[...]` (collections, last pause, objects marked, live heap), `loop[...]` (event-loop wake-ups per second, events
  per wake-up, busy share average and maximum), `tune[...]` (the loop batching setting, see `LOOP_TUNE`), `pg[...]`
  (the Postgres link as the kernel sees it, from TCP_INFO of the lanes' sockets: smoothed RTT, largest RTO, segments
  retransmitted per second, sockets in retransmit backoff, unacked segments; next to it `dbr_slow` and `dbw_slow`, the read
  and write jobs that took 150 ms or more), `net[...]`
  (kernel counters over the interval, from `/proc/net`: TCP segments per second in and out, retransmits and timeouts,
  listen, backlog, receive-queue, softnet and NIC drops, softnet squeezes, the NIC's packet and byte rates, loopback left
  out), `cpu[...]` (busy, softirq, steal and iowait share of all CPUs, the busiest CPU with its softirq share, the CPU that
  takes most NET_RX softirqs and its share, context switches and interrupts per second, and the box's CPU µs per
  request) and `thr[...]` (CPU cores per thread role from `/proc/self/task`: http loops, Postgres read and write lanes,
  other, each with its system-time share). On a loaded box they show where the CPU goes and whether the receive path
  starts dropping packets or saturates one CPU before the box is busy. A window whose p99 reaches 150 ms, or that had
  slow Postgres round trips, shed requests or 5xx, is followed by one short `knee:` line with the numbers that matter
  at that moment (p50/p99, Postgres tail and retransmits, TCP retransmits and drops, the busiest CPU and the CPU that
  takes the NIC's receive work, box CPU, loop maximum, connections), so a log excerpt of it alone tells the story.
- **Loop batching** (`LOOP_TUNE`): after a wake-up that carried fewer than 32 pieces of work (ready sockets plus tasks
  posted by other threads) a loop sleeps before polling again, so one wake-up serves what arrived meanwhile, and threads
  that post work meanwhile skip the eventfd write. The other top entries use a fixed 2 ms. What it is worth depends on the
  box (a wake-up is cheap on bare metal and costs an interrupt in a VM), so the server starts at that 2 ms and looks for a
  better setting while it runs, built never to be the reason a run fails:
  - It only explores while the server is comfortable: enough requests, p99 under `TUNE_COMFORT_MS` (100 ms), nothing shed,
    the CPUs under 70% busy. Near the knee it holds what it has.
  - A probe tests one alternative (off, 1, 4, 6, 8 ms) against the held setting over eight epochs of 5 s in the order
    A B B A B A A B (equal mean positions, so a drifting load cancels) and compares the CPU per request of the CPUs this
    process may run on (busy + softirq + steal ticks from `/proc/stat`). It is abandoned at once, back to the held
    setting, when any of its epochs is not comfortable or an epoch of the alternative has more than twice the p99.
  - The 2 ms is the prior: an alternative must be `TUNE_MARGIN_PCT` (8%) cheaper to replace it, and the prior comes back
    on a 1% win. A decision made at one load is dropped, back to the prior, when the load later moves by more than 35%,
    because what batching is worth grows with the load.
  Every decision is printed as a `tune:` line.

Reading the line on the box, to find what limits it (no tool beyond the log is needed):

| You see | It means |
|---|---|
| `cpu[hot=cpuN:9x% (soft 7x%) netrx=cpuN:9x%]` while `box=` is low | one CPU takes all the NIC receive work and is saturated: the single-queue receive path, not the server, is the limit |
| `net[... drop(listen>0 / backlog>0 / rcvq>0)]`, `retrans` or `rto` rising | the kernel drops or retransmits before the server sees the data: accept queue, socket backlog, receive buffer or the link |
| `net[... squeeze>0 softnet>0 nic_rx>0]` | the receive softirq runs out of budget, or the NIC ring or the device drops |
| `pg[... retrans/s>0 in_backoff>0]`, `dbr_slow>0` (`dbw_slow>0`: commits) | the link to Postgres loses segments: a pipelined connection stalls for at least the 200 ms minimum timeout, with every read queued behind it |
| `net[nic=.../...MB/s]` near 100 to 125 MB/s | the box's network port is full (a replica of the official rig saw about 1 Gbit/s of rows from Postgres at the knee) |
| `loop[busy=..%(max 9x%)]` | an event loop thread is saturated (or one loop has more connections than the others) |
| `thr[http=.. pg-read=.. other=..]` with a high `s..%` | where the CPU goes, and how much of it is the kernel (system time) |
| `gc[pause=..ms marked=..M]` with big pauses | the collector: its pause adds to every request that is in flight |
| `dbq=a/b` growing, `shed>0`, `dbr_p99` high | Postgres or the lanes are the limit |
| `cpu[... steal=..%]` | the hypervisor takes time from this VM |

```bash
sudo bash install.sh
bash build.sh
DATABASE_URL=postgres://feed:...@db:5432/feed JWT_SECRET=million-users-challenge HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

Thread and pool sizes default from the CPUs the process may run on (`sched_getaffinity`). The server never pins
threads to CPUs.

## What we found, and what we took

Found by measuring (profiles and interleaved A/B runs in our lab: the server on 4 CPUs at 45k req/s, the load of a
million users; the numbers are in the settings table and the table below):

- **Per-call setup, not compression, was what gzip cost.** zlib level 1 took 24.5 us per feed response; the feed is about
  half of the requests, so gzip was roughly a third of the process CPU per request (31.0 us). At 4 KB the libraries lose
  to their own setup: libdeflate level 1 14.1 us, zlib 14.5 to 16.8, zlib-ng level 1 8.7 (its `deflateReset` alone is 3.7).
  The small-input encoder (the deflate_quick / libslz design, credited above) takes 4 to 7 us and brought the process
  CPU per request to 23.1 us.
- **Round trips and wake-ups cost more than the queries.** A Postgres lane that waits for more jobs (at most 2 ms, never
  past the age of the oldest queued job) went from 1.4 to 11 reads per round trip and cut server CPU per request by 13%
  and Postgres CPU by 10%. The curve is smooth: window 0 / 0.5 / 1 / 2 ms gave 35.1 / 32.1 / 31.1 / 29.1 us of process CPU
  per request. The two top entries pull the same lever in their event loops (c-epoll-unorig and java-epoll sleep up to
  2 ms after a light iteration); here it sits at the Postgres lanes.
- **Scheduling policy**: `SCHED_BATCH` on every thread we start, 4 to 5% less process CPU per request in 5 of 5 pairs.
- **One Sync per read batch, not one per statement.** With a Sync after every read, Postgres flushed one small packet per
  statement. On loopback the total was 4.49 TCP segments per request against 3.44 with one Sync per batch (the client
  side is 3.17 of both), so the Postgres leg went from about 1.3 segments per request to about 0.3. Two interleaved
  pairs, 4 CPUs at 45k req/s: process CPU per request 21.9 -> 19.7 us (-10%), Postgres CPU -3%, p95 7.8 -> 4.2 ms.
- **A larger gather minimum**: 32 reads instead of 16 before a lane stops waiting (the 2 ms window still bounds the
  wait): 10.6 -> 15.8 reads per round trip, process CPU per request -7%, the server's CPUs -8%, Postgres CPU -12%,
  p95 4.2 -> 2.8 ms (two pairs, on top of the single Sync); segments per request 3.44 -> 3.40.
- **Where the CPU goes**, from the new `thr[...]` block at 45k req/s on 4 CPUs: the Postgres read-lane threads use 0.74
  of the 1.1 cores the process uses (a lane renders the JSON, compresses and sends the response of every handler it
  resumes), the HTTP loops 0.31 (8% busy, 1.4 events per wake-up), and about half of the lane threads' time is system
  time (receive from Postgres, send to the client).
- **Overload**: at about twice the 4-CPU capacity (156k req/s offered, 80 to 90k served) the server stays up, answers 500
  at the queue cap (0.2% of requests) and prints a `knee:` line per window with the Postgres tail, the busiest CPU and the
  queue; p99 reaches 1.5 s on the client side.
- **A million connections cost CPU too.** 900,000 idle keep-alive connections held by a separate process plus 100,000
  active users at 44k req/s on four CPUs: pass, p95 27 ms, p99 77 ms, 0 errors, 2.6 of 4 cores busy. Process CPU per
  request was 38.1 us instead of 31.0 (cold socket and connection state), and the Kotlin/Native collector's longest pause
  was 85 ms with about 4M objects marked; both grow with the connection count.
- **Packets**: at 10k req/s, 1.00 data packet per response (the gzipped feed with its headers is one segment), 2.07 packets
  in and 1.10 out per request on the client side, no retransmits, no drops: nothing left to save there. The Postgres leg
  is where packets are saved (one Sync per read batch, above). The `net[...]` block of the stats line shows the same on
  the real box.
- **Rejected after measuring**: libdeflate (about 15% faster than zlib on the feed, not 2 to 3 times), zlib's fixed-Huffman
  strategy (1 us saved, 20% bigger output), zlib's `memLevel` (no effect), zlib-ng's fastest level (2 times slower than
  ours), eight read lanes instead of sixteen at 4 CPUs (same CPU per request, p95 noisy), and a fixed 2 ms wake-up window in our own event loop: it moves the process CPU per request by -3% to -11% but the
  total of the server's CPUs by +3% to +10% here (on loopback the load generator's receive work lands on the server's
  CPUs), so `LOOP_TUNE=auto` switches it off on this machine within a minute; the leaders and the replica below report
  large gains in VMs, which is why it is a setting that tunes itself on the box.

Taken from others, not ours: the shape of the design (an epoll loop per core, hand-written HTTP/1.1 and Postgres wire
protocol, pipelined reads and group-committed writes), which every top entry shares; the small-input gzip design; wake-up
batching as a lever; one Sync per pack of reads (c-epoll, zig and c-iouring do it, and PR #26 counted 0.15 to 0.4
Postgres packets per request for the entries that pack against 1.4 to 1.5 for the ones that Sync every statement); and the
evidence for the connection count. PR #56 (nish-std-amritk) measured a replica of the official boxes on the same VPS type:
48 read connections held a million users at 16 cores where 128 and 448 did not (more connections, more and smaller
packets, more lost on the Postgres link), a 2 ms wake-up window took the server from 207% to 153% CPU and read p95 from
2.1 s to 0.5 s at 1.1M users on 8 cores, and packing reads into fuller packets cut the database box's retransmits from
about 2,600 to 1,500 a second. Those numbers are theirs; the ones above are ours.

Not found: the 1.2 to 1.4 million users where the top entries stop, with CPU not maxed (the benchmark's comments say
Postgres about 30% and the load generators under 20%). The best-supported explanation in the other PRs is packet loss on
the link to Postgres (a lost segment stalls every query pipelined behind it for at least the 200 ms minimum timeout)
and possibly the one-queue receive path of the VM's network card; nobody has measured either on the real box. The `pg[...]`,
`net[...]` and `cpu[...]` blocks and the `knee:` line of the stats line are there to show on the real box which one it is.

## Settings and why

| Setting | Default | Why |
|---|---|---|
| `HTTP_THREADS` | CPUs | one epoll loop per CPU; idle connections cost no thread |
| `DB_READ_CONNS` | 4 x CPUs, 4..48 | each connection pipelines everything queued. At most 48: on a replica of the official boxes (PR #56, see below) 48 read connections held a million users at 16 cores where 128 did not, because more connections spread the same rows over more, smaller packets and the Postgres link lost more of them |
| `DB_READ_SYNC` | `batch` (`each` = one Sync per statement) | see Design. Lab, 4 CPUs at 45k req/s, two interleaved pairs: process CPU per request 21.9 -> 19.7 us, p95 7.8 -> 4.2 ms, TCP segments per request 4.49 -> 3.44 |
| `DB_WRITE_CONNS` | CPUs / 2, 4..8 | group commit: one round trip and one WAL flush for every write queued meanwhile; at least 4 because 2 saturated on the WAL flush in the author's previous lab |
| `DB_READ_QUEUE_MAX` / `DB_WRITE_QUEUE_MAX` | 65536 / 8192 | bounded: past it requests get 500 at once, so overload never wedges the server; far above the ~21k in flight at the knee (42k req/s x 0.5 s) |
| `DB_GATHER_US` / `DB_GATHER_MIN`, `DB_WRITE_GATHER_US` / `DB_WRITE_GATHER_MIN` | 2000 / 32, 2000 / 4 | gather window: a lane that wakes to fewer than MIN queued jobs waits for more, but never longer than the window after the OLDEST job was queued (so the added latency is at most the window). Fuller pipelines and commits mean fewer round trips, syscalls and lane wake-ups per request. Lab, 4 CPUs at 45k req/s (the load of a million users): server CPU per request -13%, Postgres CPU -10%, reads per round trip 1.4 -> 11, tails unchanged; 4-8 ms gained little more. A minimum of 32 instead of 16 (with one Sync per batch, two interleaved pairs): process CPU per request -7%, the server CPUs -8%, Postgres CPU -12%, p95 4.2 -> 2.8 ms. `0` turns it off |
| `DB_OPTIONS` | `-c plan_cache_mode=force_generic_plan` | session-level only (rule 2): no re-planning of the prepared statements |
| `GZIP_LEVEL` / `GZIP_MIN_BYTES` | 1 / 1024 | the zlib fallback's level; small post bodies gain little and go out plain |
| `GZIP_IMPL` | `fast` (`zlib` = off) | the small-response encoder (see Design). Lab, 4 CPUs at 45k req/s, interleaved: gzip 24.5 -> 6.9 us per response, process CPU per request 31.0 -> 23.1 us, server cores 2.43 -> 2.11, tails unchanged within noise |
| `SCHED_BATCH` | on (`0` = off) | every thread we start runs under `SCHED_BATCH` (the Kotlin/Native runtime's own GC threads keep the default policy), so a thread woken by epoll or a futex does not preempt a running one: fewer context switches on a saturated box, no privileges needed. Lab, 4 CPUs at 45k and 70k req/s, interleaved on/off: 4-5% less process CPU per request in 5 of 5 pairs, throughput unchanged, tails unchanged within noise |
| `TCP_THIN` | on (`0` = off) | `TCP_THIN_LINEAR_TIMEOUTS` and `TCP_THIN_DUPACK` on the listeners (inherited by accepted sockets) and the Postgres sockets: a connection with few packets in flight (a response of one segment, a batch of Postgres requests) retransmits on linear instead of doubling timeouts and after the first duplicate ACK, so a segment lost twice no longer costs 400 and then 800 ms. It only changes this side's retransmissions. Not measured in the lab (no loss there); it is aimed at the retransmits the replica showed on the real network |
| `LOOP_TUNE` | `auto` | event-loop wake-up batching (see Design): `auto` starts at 2 ms and tunes it online, `on` = fixed 2 ms, `off` = never sleep, a number = fixed microseconds |
| `TUNE_EPOCH_S` / `TUNE_COMFORT_MS` / `TUNE_MARGIN_PCT` | 5 / 100 / 8 | seconds per tuner epoch; the p99 (ms) under which the server counts as comfortable (probes only run then, and stop above it); how much cheaper an alternative must be to replace the 2 ms prior |
| `STATS_INTERVAL_S` | 10 | one stats line on stdout per interval (see Design) |
| codegen | the build machine's CPU | `build.sh` asks Kotlin/Native's clang for `-march=native` (CPU name + feature list): the box builds what it runs |

## Measured locally

An i7-13700H laptop (hybrid; about 1.2x faster than the box in an earlier challenge). The server was limited to the
given CPUs with taskset (a local tool only). Postgres 18 ran in Docker on other CPUs, with its data on tmpfs and
behind a 250 µs netem delay. `bench/mload` ran with `-gzip -think-scale 2.5` on the remaining CPUs, all on one
machine.

| Run | Result | Server CPU per request (+ kernel softirq) | Memory |
|---|---|---|---|
| 4 CPUs, 150,000 users (5.7k req/s) | pass, p95 0.7 ms, p99 1.2 ms, 0 errors | 46.6 µs (+ 10.9 µs) | 109 MB |
| 8 CPUs, 150,000 users | pass, p95 0.9 ms, p99 4.2 ms, 0 errors | 60.2 µs (+ 15.6 µs) | 143 MB |
| 262,123 idle keep-alive connections (after one request each) | held | - | 96 MB, ~400 B per connection |

One machine cannot hold both ends of a million connections, and the host's conntrack table stopped at 262,144. So
1M is extrapolated: about 42k req/s x ~58 µs is ~2.4 of the laptop's cores for the server and its network
processing, and ~0.4 GB of memory.
