# c-uring-georgiannElskamp: C, one single-threaded io_uring worker per CPU, its own Postgres wire client and gzip encoder

- **Language**: C (gcc 13 from Ubuntu 24.04's `build-essential`, `-O2 -march=x86-64-v2 -flto`)
- **Framework / IO**: none. Raw `io_uring` system calls (no liburing): multishot receive into provided buffers,
  sends on the ring, registered files. An epoll loop is the fallback where rings can't be created (CI's runner).
- **Driver**: none. The Postgres v3 wire protocol is spoken from `src/main.c`: SCRAM-SHA-256 (or cleartext/trust),
  Parse once, one Bind + Execute per request, DataRows read in place.
- **Dependencies**: none beyond libc and the kernel headers. SHA-256/HMAC (`src/sha256.h`), the JSON reader
  (`src/json.h`) and gzip (`src/gz.h`) are in this folder.
- **Front**: direct (no Nginx), `PORT` and `EXTRA_PORT`, `SO_REUSEPORT` listeners per worker on both.
- **Postgres connections**: at most 64 in all (per worker: `48 / workers` for reads, `16 / workers` for writes, 2..8 and
  1..4): 24 on 2 cores, 48 on 4, 64 on 8 and on 16.

## How it works

- **A process per CPU, sharing nothing.** The parent opens the listeners (one `SO_REUSEPORT` socket per worker and
  port) and forks one single-threaded worker per CPU in its affinity mask. A worker has its own ring, its own
  clients and its own Postgres connections; a connection stays with the worker that accepted it. Separate processes
  rather than threads: nothing is shared, so there are no locks, no shared descriptor table, and no TLB shootdowns
  between workers. If a worker dies the parent forks a new one on the same listeners. One worker per CPU exactly:
  on a 2-CPU box, 1 worker collapses at the limit and 3 or 4 hold less than 2 (measured below).
- **Passes of 4 ms.** A pass takes everything that arrived since the last one. In order: requests are parsed in
  place from the ring's buffers and **go to Postgres first** (reads as one batch under one `Sync`, writes as one
  transaction), so Postgres works on them while the worker builds the answers to the previous pass's requests;
  answers are queued as sends and handed to the kernel in chunks of 128, so a long pass does not hold its first
  answers until the last is built; if the pass took more than 1.5 ms, the replies Postgres has sent meanwhile are
  taken in the same pass. One `io_uring_enter` then submits what is left and waits out the rest of the 4 ms (or
  until 1,024 completions are in): about one system call per pass for all of a worker's clients (the single-call
  turn of swift-iouring-akbashev, #112), plus one `recv`/`send` per busy Postgres connection. A kernel that wakes
  that wait early is detected and the rest is slept out. An idle worker waits for the first event instead. 4 ms
  rather than 2: fewer, larger bursts cost about 5% less CPU on the box and take load off the CPU that serves the
  NIC (rust-iouring-mehrojmajidov, #125, measured the same); 3, 6 and 8 ms were no better than 4.
- **Clients on the ring.** One multishot receive per connection (8,192 provided buffers of 2 KB per worker; a
  buffer is taken only when bytes arrive and goes back as soon as its request is parsed, so an idle connection holds
  none). Responses are one send each, written into memory that stays put until the send completes. Each client
  socket is put into the ring's registered-file table (slot = its descriptor number), so a receive or a send does
  not take and drop a file reference. A connection is 32 bytes of user memory; buffers exist only while a request
  is split across packets or pipelined behind another. New connections are taken with `accept4` once per pass
  (`TCP_DEFER_ACCEPT`: the first request is already there and is served in the same pass).
- **Ubuntu's 6.8 kernels.** `IORING_REGISTER_PBUF_RING` is registered normally first; on `EINVAL` it is tried again
  with a nonzero first reserved word, which the 6.8.0-13x/-14x kernels want (their check is inverted, Launchpad
  #2162843; the workaround of fsharp-kestrel-uring-delneg #105 and swift-iouring-akbashev #112). If the ring can't
  be set up at all, the worker runs the epoll loop. Everything below was run on 6.8.0-147.
- **Postgres.** The five statements of SPEC.md, exactly, plus `SELECT 1` for `/health`, prepared once per
  connection; one Bind + Execute per request with binary parameters and no Describe, so a reply is BindComplete,
  DataRows and CommandComplete. A post's `id` and `like_count` come back as text (shorter than 8 and 4 binary
  bytes, and they go into the JSON as they are) and `created_at` as binary (8 bytes instead of 29), as in
  c-epoll-unorig-username and csharp-epoll-xeonfx (#114).
  - **Reads**: a pass's reads go to the read connection with the fewest requests waiting, 64 to a `Sync`, then on
    to the next connection. Each is answered as soon as its rows are in. If one fails, the reads Postgres skipped
    behind it are sent again.
  - **When Postgres is the limit** (which is what the 8- and 4-core scores look like: about the same for every
    entry, 8 cores no better than 4). A read connection carries at most 96 requests; past that, reads wait in the
    server, where the order can still be chosen. They leave in arrival order while they have waited less than
    250 ms. One that has waited longer steps aside (it is parked), so the ones behind it stay fast: a database
    that falls behind then delays a few requests a lot instead of every request a little, and the run is judged
    on p95 and p99. Parked reads are sent when nothing else is waiting, and in any case before they are 45 s old
    (the load generator gives up at 60 s), oldest first: every request is answered. Measured below: with the
    database box cut down until it is the limit, plain arrival order fails at 1.35M users (the surge at the end
    of the ramp leaves a backlog that everyone then waits behind) and this passes 1.40M.
  - **Writes** (group commit): a pass's new posts and likes go out as one implicit transaction (one `Sync`, one
    commit) on a write connection that has nothing out; while every write connection waits for its commit, writes
    queue and leave together. **Every write is answered only after the `ReadyForQuery` that follows its commit**
    (rule 8). If a write fails (a like on a missing post: `23503` -> 404), Postgres rolls the group back: that
    write gets its error and each of the others is sent again in a transaction of its own.
  - A lost connection is reconnected about once a second; the reads it owed are sent again elsewhere (at most 3
    times), its writes are answered 500 (their commit may have happened: a write is never sent twice). A
    connection that owes replies and is silent for 10 s is treated as lost.
- **Three packets per request, and what happens when an answer is late.** Accepted sockets inherit `TCP_NODELAY`,
  `TCP_THIN_LINEAR_TIMEOUTS` and delayed-ACK mode (`TCP_QUICKACK` off) from their listener, so a request's ACK rides
  on its response: request in, response out, the client's ACK in (the technique of #100 and #114). The kernel
  waits 40 ms for that; past it, it acks the request in a packet of its own and leaves the mode. Two things follow:
  - **Fresh first.** At the limit this is what tips a box over: the worker falls behind, every answer is late,
    each late answer costs an extra packet, and the extra packets keep the worker behind (measured: 3.07 packets
    out per request instead of 1.96). So an answer that is already 30 ms old when its rows arrive steps aside (its
    rows are kept) for the ones that can still make it, and is sent when the pass has time, oldest first. The idea
    is bend-epoll-wyrmkeep's newest-first order (#113), applied where the answers are built.
  - **A pass has a time budget.** A worker that is behind used to take everything that had arrived, so its
    passes grew (4 ms became 30 to 45 ms in the logs), every request waited for the pass before it, and the box
    tipped over. Now, once a pass has run 12 ms, its remaining answers queue with the late ones, so the next pass
    starts on time (6, 8 and 12 ms held the same load; 12 defers the fewest answers below the limit). An answer that has waited its turn for 400 ms is parked: it goes out in time the worker would
    otherwise sleep, and in any case before it is 45 s old. On two hyperthreads at 1.60M users this is the
    difference between p95 1.5 to 2.8 s (two runs) and p95 232 ms, p99 434 ms.
  - **The mode is put back through the ring.** A late answer's socket gets `TCP_QUICKACK` 0 again by a socket
    command queued just ahead of its send (`SOCKET_URING_OP_SETSOCKOPT`, kernel 6.7+, as c-ammarserver does): no
    system call at the moment the worker has least time. On a kernel without it the fix is queued and done by
    system call in a pass that has time to spare.
- **gzip that knows what it is compressing** (`src/gz.h`). Only the feed is worth compressing (about 3.7 KB, three
  TCP segments; one when gzipped). A general compressor spends most of its time finding matches. The feed is 20
  objects with the same keys, and the server has just written that JSON itself, so it records where each key run
  (`},{"id":`, `,"body":"`, ...) starts, and the encoder emits each one as a match against the same run in the
  previous post at a distance it already knows, extended over whatever follows and is equal too (the id's leading
  digits, the timestamp's date and hour). Only the two free-text fields (body, author) go through a small hash
  matcher. One deflate block, fixed Huffman codes (four ASCII literals go out as one 32-bit write), CRC-32 by
  carry-less multiplication (checked against the table version at start-up). Any feed compresses correctly,
  whatever is in it; the layout only decides how cheaply. Measured on a Ryzen 5 7600X: a feed of load-test posts,
  3.7 KB in **1.9 µs**, a feed of seed posts, 4.8 KB in 3.0 µs (zlib level 1 takes 53 µs for the first). Under
  `mload` the feed response, head included, is 978 to 1,080 bytes (300 samples), so it is always one segment.
  300,000 random feeds (control characters, quotes, UTF-8, 2,000-byte runs, 1 to 20 posts) round-trip through
  zlib's inflate. Nothing is kept between responses: each one is compressed from its own bytes.
- **Scheduling.** Workers run in `SCHED_IDLE` (as #100 does): on a shared core the kernel's packet processing
  never waits behind the server. It is the workers' own policy; no affinity is set and nothing is pinned.
- **HTTP.** Requests are parsed in place; pipelined requests are answered in order, one at a time; a client that
  half-closes still gets the answers to what it sent; `Expect: 100-continue`, `Connection: close` and HTTP/1.0 are
  honoured. Idle connections are never timed out.
- **Auth.** HS256 with the secret's inner and outer pad states computed once; signature (constant-time compare),
  `alg` and a required `exp` on every request, then `sub` and `username`. Nothing about a token is remembered.
- **JSON.** The request-body and claims reader validates the whole document (strict RFC 8259) and keeps only the
  places of the members asked for; `body` is trimmed as JavaScript's `String.trim` does and counted in code points.
  Output strings are escaped 16 bytes at a time.

## Rules notes

- **Rule 3**: the SQL is SPEC.md's, one statement per request, prepared and pipelined; reads share a `Sync`, writes
  share a transaction. The JSON is built in the server from the rows. Session setting on its own connections only:
  `plan_cache_mode=force_generic_plan` (in the startup packet).
- **Rule 5**: 64 Postgres connections at most.
- **Rule 7**: nothing is cached across requests: no responses, no rows, no token verifications. The compressor's
  tables are the fixed Huffman codes; its hash table is cleared for every response. A late or parked answer's
  rows are held for that one request only, until it is answered.
- Every request gets its answer: a parked one is sent before it is 45 s old, which is inside the load generator's
  60 s. Nothing is dropped, refused or answered from another request's rows.
- **Rule 8**: a write's response waits for the `ReadyForQuery` after its commit.
- **Rule 9**: signature and `exp` checked on every authenticated request.
- **Rule 12**: no dependencies to pin; `install.sh` installs `build-essential` only, `build.sh` compiles `src/`.
- **Rule 13**: only settings of its own processes and sockets (`RLIMIT_NOFILE` soft limit raised to the hard one,
  `SCHED_IDLE`, socket options, its own ring's registered files). No affinity, no `/proc` or `/sys` reads, nothing
  box-wide.
- Environment: only `DATABASE_URL`, `JWT_SECRET`, `HOST`, `PORT`, `EXTRA_PORT` are needed. For experiments:
  `FEED_IO=epoll` forces the fallback loop, `FEED_PAUSE_US` sets the pass length (default 4000),
  `FEED_SCHED_IDLE=0` keeps the default scheduling class, `FEED_WORKERS` overrides the worker count,
  `FEED_LATE_MS` moves the fresh-first threshold (0 sends every answer through the late path, as the tests below
  do once), `FEED_BUDGET_US` the pass budget (default 12000, 0 = none), `FEED_PARK_MS` and `FEED_HOLD_MS` how long
  an answer and a read wait their turn before they are parked (400 and 250, 0 = never), `FEED_RESCUE_MS` the age
  at which a parked one is sent regardless (45000), `FEED_FLIGHT` the most requests on one read connection (96),
  `FEED_RCONNS` / `FEED_WCONNS` the Postgres connections per worker, `FEED_DEBUG=1` logs passes, answer ages,
  what was parked and Postgres round trips every 5 s.

## Measured on rented machines (2026-11-14)

Not the benchmark's rig either, but three separate boxes in one zone of a public cloud, set up from this
repository: the **app box** (8 vCPUs, Xeon Cascade Lake at 3.1 GHz, **kernel 6.8.0-147-generic**, a **one-queue
virtio NIC**, `box/setup-app-box.sh` and nothing else, entries installed with their own `install.sh` / `build.sh`
and started by `box/challenge.service`), the **database box** (16 vCPUs, local NVMe, Postgres 18 with
`box/postgresql.conf`, the seed, `box/reset-db.sh` before every run) and a **load generator** box. CPUs are taken
offline on the app box to get a smaller one, RPS set over the ones that remain.

Three things to know when reading the numbers:

- The load generator is the C program described under "Measured locally" (`mload`'s user model, percentiles and
  pass rule, Go's keep-alive scaled with the think time). The cloud's firewall tracks at most 1.04M connections
  per machine, so runs use think scale 1.5: one connection does the requests of 1.67 users. **User counts below
  are scoring-run users** (connections x 1.67).
- A run has the scoring's shape: 80% of the users join quickly, the last 20% at 3,300 users per second, then a
  2-minute hold, and **every request of the run counts** toward p95, p99 and the error rate.
- The cloud's network lost about 1% of the request packets on their way to the app box (no counter in either
  guest shows them; the client retransmits after ~210 ms). Every entry therefore has a p99 of 220 to 250 ms there
  even when idle. p95 is not affected.

**Two CPUs** (the two hyperthreads of one core: these cores are about twice as fast as the benchmark's, and this
is the setting whose limit, ~65,000 requests per second, is nearest the 2-core box's):

| users | this entry | this entry without the pass budget and parking | c-ammarserver-AmmarkoV (#116's head) |
|---:|---|---|---|
| 1,500,000 | | | pass: p95 169 ms, p99 540 ms |
| 1,550,000 | pass: p95 56 ms, p99 253 ms (budget 6 ms: 63 / 239 and 196 / 393) | pass: p95 40 ms, p99 251 ms; again: 218 / 652 | fail: p95 4.7 s, 37% errors |
| 1,600,000 | pass: p95 232 ms, p99 434 ms (budget 6 ms: 239 / 413) | fail: p95 1.5 s; again: p95 2.8 s | |
| 1,650,000 | fail: p95 397 ms, p99 2.7 s (budget 6 ms) | | |

At equal load below the limit (steps of a rising run, think scale 1.0) this entry used 22.8 to 25.2 µs of CPU per
request against 24.7 to 28.6.

**Two whole cores** (think scale 1.0, the same run shape): this entry passes 2,000,000 users (p95 221 ms, p99
425 ms) and fails 2,100,000 (p95 306 ms, p99 1.3 s); c-ammarserver passes 1,900,000 (p95 75 ms, p99 297 ms) and
fails 2,000,000 (p95 421 ms, p99 1.2 s, 0.4% errors). In a rising run below the limit: 18.7 to 20.4 µs of CPU
per request against 20.4 to 22.0. **Eight CPUs**, 2,500,000 users (105,000 requests per second): p95 27 ms, the
app box a third busy, the database box two thirds.

**When the database is the limit.** The leaderboard's 8- and 4-core scores are within a few percent of each other
for every entry near the top and 8 cores are never better than 4, which is what a full database box looks like
from outside. To have that here, the database box was cut to 6 CPUs (it then saturates at about 56,000 requests
per second) and the app box given 4 whole cores (a third busy):

| users | this entry | this entry, plain arrival order | bend-epoll-wyrmkeep (#113) | c-ammarserver-AmmarkoV |
|---:|---|---|---|---|
| 1,250,000 | | | | pass: p95 46 ms, p99 179 ms |
| 1,300,000 | | | pass: p95 79 ms, p99 342 ms | fail: p99 3.6 s |
| 1,350,000 | | fail: p95 1.0 s | fail: p99 1.8 s | |
| 1,400,000 | pass: p95 291 ms, p99 325 ms (twice) | fail: p95 1.2 s | | |
| 1,450,000 | fail: p95 309 ms, p99 40 s | | | |

bend holds 4% more than c-ammarserver here, as on the leaderboard's 4-core column (1.51M and 1.45M). Two things
make up this entry's margin: the saturated database gets through about 56,700 requests per second under it
against 54,200 to 55,700 under the three others measured (bend, c-ammarserver, c-epoll-unorig-username), and
the hold / park / rescue order described above. With the database saturated, its throughput did not depend on
this entry's number of read connections (8, 16 or 32 in all; 56 cost 4%) or on the pass length (2, 4, 8 ms).

**The confirming run's shape: the whole ramp at 3,300 users per second, then a 5-minute hold.** The tables above
are 2-minute holds, where the ramp's requests are about 60% of the run. With a 5-minute hold the hold is more than
half of it, so each entry's highest pass was run again this way, then one step to either side:

| | this entry | the other entry |
|---|---|---|
| two CPUs (hyperthreads) | 1,600,000 pass: p95 194 ms, p99 389 ms; 1,650,000 fail: p99 3.2 s | c-ammarserver: 1,450,000 pass: p95 36 ms, p99 225 ms; 1,500,000 fail: p99 1.7 s, 0.4% errors |
| two whole cores | 2,000,000 pass: p95 218 ms, p99 417 ms | c-ammarserver: 1,900,000 pass: p95 87 ms, p99 286 ms |
| database the limit | 1,400,000 pass: p95 288 ms, p99 328 ms; 1,450,000 fail: p99 45 s | bend: 1,300,000 pass: p95 69 ms, p99 266 ms; 1,350,000 fail: p99 5.5 s |

Not every pass is equally comfortable. In the 60 five-second slices of the hold, p99 was over a second in 1 slice
for this entry at 1,600,000 on hyperthreads, in 12 at 2,000,000 on whole cores and in 17 at 1,400,000 with the
database as the limit (that is the parked requests coming out, and it is what the design trades for the low p95);
for bend at 1,300,000 in 14, for c-ammarserver at its passes in 0 and 1. On whole cores neither entry was run a
step above its pass with a 5-minute hold; with 2-minute holds this entry failed 2,100,000 and c-ammarserver
2,000,000.

- `test/run.sh` against the app box from the load generator: `passed=42 failed=0` on the ring and with
  `FEED_IO=epoll`, started by the box's unit.
- Tried there and left out: steering new connections to the less busy worker (each worker publishes how much of
  the time its passes take; a busier one leaves `accept` to the others). The kernel moved each worker between the
  two CPUs 3 to 6 times a second, so there was nothing stable to balance. Newest-first for waiting reads: p95
  stays low but more than 1% of the requests end up waiting tens of seconds.

## Measured locally

Not the benchmark's rig, but built to be like it: an **Ubuntu 24.04 VM on kernel 6.8.0-147 with 2 vCPUs** (each
on a host core of its own, QEMU/KVM on a Ryzen 5 7600X), a **one-queue virtio NIC** (vhost-net), set up with the
commands of `box/setup-app-box.sh` (RPS over both CPUs, RFS, `fq`). Entries are installed with their own
`install.sh` (root) and `build.sh` (user `challenge`) and started from `box/challenge.service`. Postgres 18
(`box/postgresql.conf`, shared_buffers 2 GB) and the load generator run outside the VM; segmentation offload is
off on the host side, so Postgres' replies cross the link as 1,448-byte frames and the VM has to GRO them.

The load generator is a small C program with `mload`'s user model (same requests, think times, percentiles and
pass rule; `mload` itself needs ~40 µs of CPU and ~29 KB per user-request, too much for this machine at these
sizes). Like Go's dialer it has TCP keep-alive on, scaled with the think time, because at the scoring think time
that is about one probe and its ACK per request: with it the box sees **3.9 to 4.1 packets in and 1.9 to 2.1 out
per request**, as at `-think-scale 2.5`. "CPU per request" is everything both vCPUs did (user, system, interrupts,
softirq) divided by the requests of a steady window. The machine drifts by several percent over minutes, so
entries were run in alternation.

| 200,000 users, think scale 0.5, ~42,000 req/s, alternating runs | CPU per request | the NIC's CPU busy | the other CPU | p95 |
|---|---:|---:|---:|---:|
| this entry | 18.9 - 19.8 µs | 41.5 - 43 % | 40.5 - 42 % | 32 - 37 ms |
| c-ammarserver-AmmarkoV (#116's head) | 20.9 - 21.9 µs | 47 - 50 % | 41.5 - 44 % | 30 - 36 ms |

#125 found that on 2 CPUs the entries' official order follows how busy the NIC's CPU is; that is the column this
entry was tuned on.

- **More connections**: 400,000 users at think scale 1.0 (~42,000 req/s): 19.2 to 21.3 µs per request against 22.0
  to 23.8. 600,000 users at think scale 1.5 (an earlier build of this entry, one run of c-ammarserver): p95 44 ms,
  p99 61 ms, no errors, against 88 and 291 ms.
- **At the limit.** With both vCPUs slowed to half speed (so the box, not the load generator, runs out):

  | users (think scale 0.5) | this entry: p95 / p99 | c-ammarserver: p95 / p99 |
  |---|---|---|
  | 165,000 | 112 / 190 ms, pass | 451 / 837 ms, pass |
  | 172,000 | 363 / 828 ms, pass | 699 / 1,062 ms, fail |
  | 176,000 - 178,000 | passes about every second run | - |

  Runs this close to the limit differ from one to the next on this machine (the same build passed and failed 176,000).
- **Tried and left out** (same rig): one worker on 2 CPUs (the NIC's CPU drops from 45% to 33% busy, but the lone
  worker collapses at the limit); 3 or 4 workers on 2 CPUs (3 holds less than 2, 4 fails where 2 passes);
  steering each connection to the worker on the CPU that receives it with `SO_INCOMING_CPU` (no gain: unpinned
  workers were moved between the CPUs hundreds of times in a run); passes of 3, 6 and 8 ms; the default scheduling
  class.
- `test/run.sh`: `passed=42 failed=0` on the ring and with `FEED_IO=epoll`, on kernel 6.18 and in the VM on
  6.8.0-147, and again on both loops with `FEED_LATE_MS=0`.
- Beyond the suite (both loops): the gzipped feed equals `test/golden/feed.json` byte for byte after `gunzip`;
  three pipelined requests come back in order; a request split over three writes; a 12 KB body; a burst of 30
  likes with 3 likes on a missing post mixed in (30 x 201, 3 x 404, 30 rows committed: the group retry);
  Postgres restarted under load (500s during the outage only, every connection back within seconds, `/health`
  200 -> 503 -> 200).
- Where the box's time goes (`perf`, cpu-clock, in the VM at ~52,000 req/s): this server's own code is about a
  tenth of the busy time (building and compressing the feed is most of that); the rest is the kernel's work per
  packet, the same for every entry: socket lookup, the virtio notifications, AppArmor's hooks on every send and
  receive, RPS.

## Running

```bash
sudo ./install.sh   # build-essential
./build.sh          # gcc: src/main.c -> bin/server
DATABASE_URL=postgres://user:pass@host:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 ./start.sh
```

## Credits

The shape is the one the leaderboard converged on, and several parts come straight from other entries' write-ups:
the pass, `SCHED_IDLE` and the delayed-ACK handling from c-epoll-unorig-username (#10, #100, #104); one system
call per pass and the 6.8 buffer-ring workaround from swift-iouring-akbashev (#112) and
fsharp-kestrel-uring-delneg (#105); per-column result formats from csharp-epoll-xeonfx (#114); registered files
and the socket option through the ring from c-ammarserver-AmmarkoV (#116); 4 ms passes and the NIC's CPU as the
thing to watch on 2 CPUs from rust-iouring-mehrojmajidov (#125); newest-first from bend-epoll-wyrmkeep (#113);
group commit with retry-alone from several of them. New here: the feed encoder that uses the JSON's own
structure, sending requests to Postgres before building the previous answers, and fresh-first applied to
answers.
