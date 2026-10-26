# Research: techniques in the examples and the other submissions

A survey of the rules, the six examples and the open pull requests (as of 2026-10-26), made for this submission. It
is the input to [FINDINGS.md](FINDINGS.md), which records what was adopted and measured. Gains quoted here are the
other entries' own claims, not our measurements. None of the submissions has an official score yet.

Date: 2026-10-26. Sources: `upstream/main` @ `8f02bc6` of https://github.com/arjaythedev/million-users-challenge
(README.md, SPEC.md, bench/README.md, box/*, examples/*), all pull requests (open and closed), the owner's YouTube
channel, and the fs2 3.14.0 / http4s 0.23.38 sources.

Base URLs: repo = https://github.com/arjaythedev/million-users-challenge ,
PR n = https://github.com/arjaythedev/million-users-challenge/pull/n .
Not surveyed: our own PR https://github.com/arjaythedev/million-users-challenge/pull/7 , the pass-criteria PR
https://github.com/arjaythedev/million-users-challenge/pull/5 and issue
https://github.com/arjaythedev/million-users-challenge/issues/6 (both ours).

## 0. The facts that drive every verdict

- **Memory per idle connection decides the score.** A million users hold a million keep-alive connections, served
  directly (rule 11, no Nginx). The request rate is small: ~42,000 req/s at a million users, ~5% writes
  (bench/README.md). On 16 cores that is ~380 µs of CPU per request, kernel included.
- **The database is never the bottleneck** (rule 3). If the database box limits a run, the owner gives it more
  resources. So Postgres CPU per request does not matter here. Only the app box's CPU and memory do.
- **The SQL is fixed** (rule 3): SPEC.md's statements, one per request. Preparing, pipelining and batching writes
  into one statement or transaction are allowed. SQL rewrites (all of multicore's big wins) are not.
- **The box** (box/setup-app-box.sh): 16 vCPU, 32 GB, no swap. One NIC queue, spread over all cores with RPS, so the
  kernel's network work runs on the same cores as the app. `tcp_mem` max is 3,145,728 pages (12 GB). `somaxconn`
  65,535. `LimitNOFILE` 2,097,152. Postgres runs with `enable_memoize = off` and `jit = off` server-wide
  (box/postgresql.conf).
- **Out of memory means you lose; a crash is not restarted** (rules 10 and challenge.service `Restart=no`).
- **The load**: ramp of 60 s or more (at most ~3,300 new connections/s), 5-minute hold, `-gzip`, think scale 2.5.
  Pass: p95 < 500 ms, p99 < 1 s, errors < 1%. A request counts as failed after 60 s.

## 1. Memory per idle connection, across stacks

This is our bottleneck, so this table collects every number found.

| Stack | Per idle connection | How it was measured | Source |
|---|---|---|---|
| Zig, io_uring with direct (fixed) descriptors | 16-byte record + the kernel socket; RSS 19 MB at 192,000 (~0.07 KB) | RSS before and after | https://github.com/arjaythedev/million-users-challenge/pull/4 |
| Java 25, own epoll loops through FFM | 72-byte `Conn` + its fd-table slot | class histogram, 60,000 users | https://github.com/arjaythedev/million-users-challenge/pull/2 |
| Rust, thread-per-core tokio, own HTTP | ~1.2 KB RSS (10.6 MB idle, 184 MB at 150,000) | RSS | https://github.com/arjaythedev/million-users-challenge/pull/1 |
| Quarkus / Vert.x on Netty epoll | ~4.5 KB live heap (~10.5 KB RSS incl. direct buffers) | full GC, 15,000 users | https://github.com/arjaythedev/million-users-challenge/pull/3 |
| Spring Boot / Tomcat NIO, 4 KB app buffers | 9.9 KB heap (18.0 KB with the default 8 KB buffers) | full GC, 50,000 idle | [examples/java](https://github.com/arjaythedev/million-users-challenge/tree/main/examples/java) |
| ASP.NET Core / Kestrel | 10-14 KB RSS | 60,000 idle | [examples/csharp](https://github.com/arjaythedev/million-users-challenge/tree/main/examples/csharp) |
| Go net/http (Gin) | ~10 KB heap + 2.5 KB goroutine stack; ~29 KB RSS at GOGC=100 | gctrace, after one gzip /feed | [examples/go](https://github.com/arjaythedev/million-users-challenge/tree/main/examples/go) |
| Rust axum / hyper | ~28 KB (16 KB read + 8 KB write buffer + task); 33.5 KB before `into_make_service()` | RSS | [examples/rust](https://github.com/arjaythedev/million-users-challenge/tree/main/examples/rust) |
| **Ours: http4s Ember on fs2 + Cats Effect** | **38.1 KB live heap** | class histogram after full GC, 20,000 idle | [README.md](README.md) |

What the low numbers have in common:

- **No buffer per idle connection.** The connection waits for readability with no buffer. It reads into a buffer
  shared per thread (Rust 64 KB scratch, Java per-loop buffers, Zig a kernel-provided buffer ring). It gets its own
  buffer only while a request is incomplete or a response is not fully sent.
- **No task, timer or pipeline state per idle connection** beyond one small object. Zig and Rust PR 1 have no idle
  timeout. Zig never touches an idle connection.
- **Small heap objects only** (Netty: a channel and a 7-handler pipeline, ~4.5 KB).

Ember keeps, per idle connection: the 8 KB fs2 read buffer (allocated with the socket in
`Socket.BufferedReads`, kept while it is open), 10 fibers, 21 fs2 scopes and 18 interrupt contexts. No Ember
setting changes that. At 38 KB, a 24 GB heap holds ~630,000 idle connections before any request garbage. Even
Tomcat's 9.9 KB would put a million connections at ~10 GB.

## 2. The six examples and the follow-ups

Scores from [examples/README.md](https://github.com/arjaythedev/million-users-challenge/blob/main/examples/README.md),
16 cores, Postgres on its own box. Nobody reached a million.

| Stack | Users | Follow-up | After | Lesson for us |
|---|--:|---|--:|---|
| C# ASP.NET Core + Npgsql | 665,000 | `DB_POOL=256` | 700,000 | Kestrel ~10-14 KB/conn. Server GC with `GCHeapHardLimitPercent` 70%: without a cap, RSS reached ~29 GB at ~800k and p95 went to 18-23 s. Npgsql auto-prepare off made Postgres use 47% CPU vs 26% for pgx: prepare statements (we do). |
| Node Fastify, `node:cluster` | 587,000 | none, "searched again" | 639,000 | Run-to-run spread is ~9%. One score is not a precise number. |
| Rust axum + SQLx | 413,000 | - | - | ~28 KB/conn. A per-connection copy of the router cost 5.6 KB until `into_make_service()`. sqlx `test_before_acquire` adds a round trip per acquire. |
| Java Spring Boot, Tomcat, virtual threads, G1 | 321,000 | `MAX_RAM_PCT=80` | 340,000 | See below: it died of in-flight requests and G1 full GCs, not of idle connections. |
| Go Gin + pgx | 248,000 | `GZIP_MW=gzhttp TCP_KEEPALIVE=off` | 650,000 | The gzip middleware took a ~1 MB `flate` writer before the handler waited for the pool. A hiccup at 350k grew the heap from 2.6 GB to ~200 GB virtual. TCP keep-alive probes every 15 s contended on the single network queue: p95 58.7 to 6.2 ms. |
| Python FastAPI + asyncpg | 213,000 | - | - | CPU-bound per worker. `SO_REUSEPORT` balanced connections far better than one shared socket. |

**The Java collapse** (examples/java README, "Heap-size experiment"): at 300k users the live heap was ~9 GB. At 360k
it was 22 GB (`Pause Full 22460M->22460M`), with 230k `Http11Processor` and 143k `VirtualThread`, ~80 KB of buffers
per processor. "Longer GC pauses mean more requests in flight, which means more live processors, which means longer
GC pauses." This is the failure mode that ZGC is meant to avoid for us. It also says: memory per **in-flight**
request matters, not only per idle connection.

**Pool sizes.** All examples use `min(128, 8 x cores)` = 128 connections. Each connection serves only ~600-1,600 req/s
(one ~0.25 ms network round trip per query). The Go review at 4 cores: pool 16 gave 9.9k req/s (p95 218 ms), pool 32
gave 12.4k (p95 94 ms). C# went from 665k to 700k with 256.

**Other settings in common:** listen backlog 65,535 (C#, Node, Rust, Python; Java 8,192), keep-alive timeout 75-130 s
or none, no access logs, heap cap 70-75% of RAM (Java 70%, C# 70%, Go `GOMEMLIMIT` 75%), gzip at the default or
fastest level.

## 3. The submissions

| PR | Entry | Idea | Per conn | Postgres | Status |
|---|---|---|---|---|---|
| https://github.com/arjaythedev/million-users-challenge/pull/1 | rust-tokio-cknutson12 | one current-thread tokio runtime per core, `SO_REUSEPORT` per thread, httparse over a per-thread 64 KB scratch buffer | ~1.2 KB RSS | tokio-postgres pipelined, 64 read + 64 write connections, least-loaded, binary results | open, no score; 50k users locally p95 3.1 ms |
| https://github.com/arjaythedev/million-users-challenge/pull/2 | java-epoll-stohirov | Java 25, no dependencies, own epoll (or io_uring) loops through FFM, own Postgres client | 72 B heap | own pipelined client on the event loop, binary, group commit, wake-up batching | open, no score |
| https://github.com/arjaythedev/million-users-challenge/pull/3 | java-quarkus-stohirov | Quarkus 3.40 + Vert.x routes on Netty native epoll, one event loop per core, no worker threads | ~4.5 KB heap | Vert.x reactive client, pipelining limit 256, 64 connections, warmed at start | open, no score; 15k users locally |
| https://github.com/arjaythedev/million-users-challenge/pull/4 | zig-iouring-harshitkhandelwal208 | Zig, raw io_uring with direct descriptors, 2 worker threads on any core count, own HTTP, own Postgres client, gzip loop in x86 assembly | 16 B + kernel socket | read "packs" (N Bind/Execute + one Sync), group commit, `enable_memoize=off` and `force_generic_plan` as session settings | open, no score; 200k connections locally, 0 errors |

No maintainer comments or reviews on any of them yet.

## 4. Techniques and verdicts

Verdicts: **ADOPT**, **HAVE** (already in our server), **NOT NEEDED** (correct but not our limit), **NOT POSSIBLE IN
EMBER/SKUNK**, **TOO GAMED** (allowed, but not idiomatic Typelevel code), **FUTURE**.

### 4.1 Connections and memory

| Technique | Who | Evidence | Verdict for us |
|---|---|---|---|
| Wait for readability with no buffer; read into a per-thread buffer | PR 1, PR 2, PR 4 | 0.07-1.2 KB per connection | **NOT POSSIBLE IN EMBER.** fs2 3.14 already waits through Cats Effect's `Selector` (`SelectingSocket.readChunk` does `selector.select(ch, OP_READ)`), but `BufferedReads` allocates the 8 KB buffer when the socket is made and holds it while waiting. **FUTURE (upstream):** allocate the read buffer on first readiness, or release it when idle. That alone saves 8.6 of 38.1 KB. |
| A small per-connection footprint: one task/fiber, no scopes | PR 1 (boxed handler future, small connection future), Netty | 1.2-4.5 KB | **NOT POSSIBLE IN EMBER** (10 fibers, 21 scopes per connection; no setting). **FUTURE:** our own HTTP/1.1 connection loop on Cats Effect: one fiber per connection that waits on `Selector` with no buffer, reads into a per-worker buffer, and calls the same `HttpApp[IO]`. A suspended fiber is ~1-1.5 KB in our histogram. Less idiomatic than Ember; ~300-500 lines. |
| Different http4s backend on Netty (`http4s-netty-server`) | (Netty: PR 3) | Vert.x on Netty: ~4.5 KB heap per idle connection | **FUTURE, measure first.** Keeps the routes and `HttpApp` unchanged. Its per-connection cost with the http4s adapter is not measured anywhere. Netty native epoll is a pinned Maven dependency (PR 3 uses it). |
| Shrink per-connection buffers | Tomcat 8 KB to 4 KB: 18.0 to 9.9 KB | examples/java | **HAVE** the nearest Ember knob (`receiveBufferSize` 8 KB; fs2 never goes below 8 KB). |
| Direct (fixed) descriptors in io_uring | PR 4 | no process fd per client | **TOO GAMED / NOT POSSIBLE** on the JVM without own native I/O. Not needed: `LimitNOFILE` is 2,097,152. |
| Heap cap below RAM, room for the kernel | C# 70%, Java 70%, Go `GOMEMLIMIT` 75%, Quarkus 50% + 6 GB direct | C# without a cap: RSS ~29 GB, p95 18-23 s | **HAVE** (75%, fixed, pre-touched). Check on a big box: heap 24 GB + JVM native memory + kernel sockets (~3-4 KB each idle, `tcp_mem` allows up to 12 GB) must stay under ~31 GB. |
| Cap connections so overload cannot OOM the process | none explicitly | rule 10: OOM loses; no restart | **ADOPTED** (FINDINGS.md): the default cap is now what fits in 80% of the heap. Before: `MAX_CONNECTIONS` defaulted to 1,000,000 per port. At 38 KB the heap fills at ~630k in total. A cap of ~275k per port stops accepting before the heap is full (connections wait in the backlog), so the process survives a failed run. The run above the cap fails either way. |
| Memory per in-flight request | Java example (~80 KB per Tomcat processor, collapse at 360k), Go gzip (~1 MB per queued request) | see §2 | **HAVE** for gzip: `Gzip.scala` compresses only the finished body, with a per-thread `Deflater` (the gzhttp lesson). **Measure** Ember's in-flight cost at the limit. |
| Infinite or long idle timeout | PR 1 and PR 4: none; examples 90-130 s | a timer per connection costs memory and CPU | **HAVE** (`Duration.Inf`). |

### 4.2 HTTP layer and kernel

| Technique | Who | Evidence | Verdict |
|---|---|---|---|
| Listen backlog 65,535 | C#, Node, Rust, Python, PR 1, PR 3 | ramp is up to 3,300 connections/s | **HAVE** (`ListenBacklog.scala`). |
| No TCP keep-alive probes | Go follow-up | p95 58.7 to 6.2 ms at 350k | **HAVE.** Ember's default `additionalSocketOptions` is `Nil` and the JVM does not set `SO_KEEPALIVE`. Even if set, Linux waits 7,200 s before the first probe. |
| `TCP_NODELAY` | PR 1, PR 2 (on the listener, inherited), Node, Go, Kestrel and Netty by default | avoids a delayed-ACK stall when a response leaves in two writes | **ADOPTED** (FINDINGS.md). Ember sets no socket options and fs2 applies only what it is given. Add `.withAdditionalSocketOptions(List(SocketOption.noDelay(true)))`. Our local p99 of ~2 ms suggests Ember writes each response in one write, so expect no change, but it removes the risk. |
| `SO_REUSEPORT`, one listener per thread or loop | PR 1, PR 2, PR 3, PR 4, Python | spreads accepts, no shared accept queue | **NOT NEEDED.** One accept fiber per port handles ~3,300 connections/s. Cats Effect's work-stealing pool spreads the connections. |
| Fewer worker threads than cores (leave cores to the kernel) | PR 4: 2 workers on any core count (15 µs/request vs 17-19 with 4); PR 3: 1 loop per core, not 2 | kernel TCP is ~70% of PR 4's CPU; RPS puts it on all cores | **FUTURE, measure on the box.** Try `COMPUTE_THREADS` 12-14 vs 16. CPU is not our limit, so low priority. |
| Wake-up / completion batching (wait up to 1-2 ms for more events) | PR 2 (kernel µs/request 12 to 6), PR 4 | p99 a few ms higher | **NOT POSSIBLE IN EMBER** (no setting in the Cats Effect selector loop). Not needed: CPU is not our limit. |
| `TCP_DEFER_ACCEPT` | PR 2 | one wake-up per new connection instead of two | **NOT NEEDED.** Possible with the same FFM `setsockopt` trick as `ListenBacklog.scala`, but saves little at 3,300 connections/s. |
| io_uring | PR 2 (no cheaper than epoll locally), PR 4 | - | **TOO GAMED** for us (own native I/O). |
| Hand-written HTTP parser, precomputed status lines, hand-written JSON | PR 1, PR 2, PR 4 | user CPU ~3 µs/request in PR 4 | **TOO GAMED.** Keep Ember and circe. |
| gzip level 1 for bodies above a threshold, compressor reused per thread | PR 1 (libdeflate, ≥ 512 B), PR 2 (`Deflater` per loop, ≥ 1 KB), PR 3 (level 1, ≥ 256 B), PR 4 (own encoder) | feed 5.1 KB to ~1.9 KB; ~2-4 µs/request | **HAVE** (level 1, ≥ 1 KB, per-thread `Deflater`, `Content-Length` kept). |
| Own gzip encoder in assembly | PR 4 | -23% on gzip | **TOO GAMED.** |

### 4.3 Postgres

| Technique | Who | Evidence | Verdict |
|---|---|---|---|
| Prepare each statement once per connection | everyone; C# without it: Postgres 47% CPU vs 26% | examples/csharp | **HAVE** (skunk caches prepares per session). |
| Session settings `force_generic_plan`, `jit=off` | PR 4 | - | **HAVE.** `jit=off` and `enable_memoize=off` are also server-wide on this box. |
| Session setting `max_parallel_workers_per_gather=0` | PR 4 | the box allows 2 per gather | **ADOPT (cheap, optional).** These are short index lookups; it removes any chance of a parallel plan. Low expected gain. |
| Session setting `enable_memoize=off` | PR 4 (Postgres µs/request 53 to 35 on its machine) | - | **NOT NEEDED:** already off server-wide in box/postgresql.conf. |
| Pool size ~128 | all examples; C# 256: +5% | each connection ~600-1,600 req/s | **HAVE** (128 read + 32 write). At ~42k req/s that is enough with margin. |
| Separate read and write connections | PR 1, PR 2, PR 4 | a read never waits behind a commit | **HAVE.** |
| Pipelining many queries on one connection | PR 1 (tokio-postgres), PR 2, PR 3 (limit 256), PR 4 | PR 4: server CPU 32.5 to 16.5 µs/request with packing and completion batching | **NOT POSSIBLE IN SKUNK** (one exchange per session, https://github.com/typelevel/skunk/issues/912). Not needed: the database is never the bottleneck, and our app CPU budget is ~380 µs/request. |
| One `Sync` for a pack of reads | PR 4 | fewer packets and backend wake-ups | **NOT POSSIBLE IN SKUNK.** |
| Binary result format | PR 1, PR 2, PR 4 | Postgres does not format timestamps | **NOT POSSIBLE IN SKUNK** (text only, https://github.com/typelevel/skunk/issues/837). Minor. |
| Group commit | PR 2, PR 4 (failed group retried one by one) | rule 3 and rule 8 allow it | **NOT NEEDED** at ~2,000 writes/s. Postgres shares one WAL flush among concurrent commits on 32 write sessions. If needed: fs2 `groupWithin` into one `session.transaction`, retry each on failure. |
| Warm all connections at start | PR 3 | avoids 64 handshakes at the start of the ramp | **HAVE** (the session pool opens all sessions at start). |

### 4.4 JVM, GC and runtime

| Technique | Who | Evidence | Verdict |
|---|---|---|---|
| G1 | Java example (collapsed with 22 GB full GCs at 360k), PR 3 (50% heap) | §2 | **REJECTED** for our live set. |
| ParallelGC, 2 GB heap | PR 2 | live set is tiny (72 B per connection) | **NOT APPLICABLE:** our live set is ~20 GB at 500k. |
| Generational ZGC | ours | our GC showcase: ZGC p99.9 110 ms vs G1 155 ms pauses and Parallel 1.7 s full GCs, at ~40% more CPU | **HAVE.** |
| Fixed, pre-touched heap | ours; Java example starts at 10% and grows | no swap on the box | **HAVE.** |
| Tracing off, starvation checker off | ours | - | **HAVE.** |
| JWT: keyed `Mac` per thread, constant-time compare | PR 2, PR 3 (one `Mac` per loop), PR 4 (key pads once) | - | **HAVE** in effect: `Jwt.scala` clones an initialised `Mac` per request and compares with `MessageDigest.isEqual`. A `ThreadLocal` `Mac` would save the clone; minor. |

## 5. Rule notes

- **Rule 3**: only SPEC.md's SQL, one statement per request. Session `SET`s (including through the startup packet)
  are allowed (rule 2). Batching writes is allowed; merging reads into one statement is not listed and changes the
  SQL, so avoid it (FINDINGS.md: "No batching of reads or writes").
- **Rule 7**: prepared statements and connections are fine. PR 4 keeps the HMAC key pads, which is a key schedule,
  not a cached verification.
- **Rule 9**: nothing that detects the load test. Wake-up batching windows are generic; tuning them to mload's think
  times would not be.
- **Rule 12**: Maven dependencies with native libraries (Netty epoll in PR 3) are dependencies, not prebuilt
  binaries of our own.
- **Rule 13**: process-level settings only. Our `listen()` backlog change and socket options are inside the process.

## 6. Ranked plan for us

| Rank | Idea | Expected impact | Confidence |
|--:|---|---|---|
| 1 | Measure on a 16-core / 32 GB box: users at the heap limit, RSS (heap + JVM native + `/proc/net/sockstat` mem), ZGC CPU and allocation stalls, in-flight heap at the limit | decides everything below | - |
| 2 | **Done.** `TCP_NODELAY` through `withAdditionalSocketOptions` | removes a latency risk; no gain expected | 0.8 |
| 3 | **Done.** Decide the `MAX_CONNECTIONS` default: a cap below the heap limit, so an overloaded run fails without an OOM crash | survival, not score | 0.6 (depends on whether the owner restarts the server between runs) |
| 4 | `max_parallel_workers_per_gather=0` as a session setting | small | 0.6 |
| 5 | `COMPUTE_THREADS` 12-14 vs 16 on the box | small, latency | 0.5 |
| FUTURE | Per-connection memory: http4s-netty backend (measure its KB per idle connection first), or our own Cats Effect connection loop on `Selector` with no idle buffer, or upstream fs2/Ember changes (lazy read buffer: -8.6 KB; fewer fibers and scopes) | the only path to a million: from 38 KB to under ~20 KB | 0.5 |

Not adopted: io_uring, own Postgres client, own HTTP parser, assembly gzip (too gamed); pipelining, read packs, binary
results (not possible in skunk, and not needed); group commit and wake-up batching (not needed at this load).

## The challenge owner's videos

Arjay McCandless ([YouTube](https://www.youtube.com/channel/UCdSsl5RM4Tne4MhDRz2HjWw)) runs this challenge.

- **"The video"** that the README refers to (six stacks on 1-16 cores, best 665,000 users) **is not public** as of
  2026-10-26. It is not in the channel's video list (newest: 2026-10-23, "I Asked Developers to Max Out a $12 Server
  (again)"), and YouTube searches for it find nothing. Its results are in examples/README.md and the example
  READMEs (§2), which we use instead.
- **[How many users can you support?](https://www.youtube.com/shorts/RNn3G01CDQQ)** (short, 2026-10-25, 67 s;
  transcript from YouTube's captions). His method, which mload implements: profile real users into a virtual user
  (session length, calls, mix), test a copy that matches production, ramp until the failure criteria, and track
  p50/p95/p99 and the error rate. "The app has to stay fast for almost everyone, and it can't break for anyone."
  Lesson for us: tails and errors decide, not throughput. A crash or OOM is the worst outcome.
- The earlier videos (the $12 and multicore challenges) are summarised in our
  [multicore RESEARCH.md](https://github.com/sideeffffect/multicore-server-challenge/blob/scala-http4s-sideeffffect/submissions/scala-http4s-sideeffffect/RESEARCH.md#the-challenge-owners-videos).
  The points that carry over: in his batch 2 ([video](https://www.youtube.com/watch?v=-BFt49ula08)) memory stopped
  entries (Go net/http and Haskell were OOM-killed), connection limits stopped four entries at 71,000, and idle
  timeouts that are too short turn keep-alive into reconnect churn. He says he would not ship most of the fastest
  entries (single-file servers, unsafe code, partial HTTP). That supports staying with ordinary Typelevel code.
