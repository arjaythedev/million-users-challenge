# C + AmmarServer + libpq, deferred responses, served direct on :80 and :81

| | |
|---|---|
| **Language** | C ( C11, gcc 13, `-O3 -march=native -flto`, for AmmarServer as well as the API ) |
| **HTTP server** | [AmmarServer](https://github.com/AmmarkoV/AmmarServer) in API mode on `io_uring`, pinned by commit in `build.sh` ( `fa7dcdfd` ) |
| **Postgres driver** | libpq 16 ( Ubuntu 24.04's `libpq-dev`, which speaks to Postgres 18 ), prepared statements, pipeline mode, binary results |
| **Compression** | ISA-L 2.31 igzip ( Ubuntu 24.04's `libisal-dev` ), level 0, feed responses only |
| **Crypto** | OpenSSL `libcrypto` for HS256 |
| **Processes** | 1 parent + `WORKERS` worker processes, half on `PORT` and half on `EXTRA_PORT`; `WORKERS` defaults to the affinity CPU count ( one worker per CPU ) |
| **Database threads** | `DB_THREADS` per worker ( default **3** : two read lanes and one write lane ; **4** , three read lanes , at 4 workers or fewer ) |
| **Postgres connections** | `WORKERS * DB_THREADS`, capped at the **512** allowed ; **16**, **24** and **48** on 4-, 8- and 16-core runs |

This is my [$12 server challenge](https://github.com/arjaythedev/twelve-dollar-server-challenge) submission
moved onto 16 cores and Postgres. The HTTP side, the routing, the JSON writing and the HS256 verification are
the same code. The previous revision kept one blocking libpq connection per worker; it scored a million users on
16 and 4 cores and 1.02M on 8, but its p95 rose before its CPU did, because the one
ring thread of each worker waited on one database round trip at a time. This revision removes that wait from
the ring thread.

## Design

AmmarServer's API mode is one thread owning every connection from `accept` to `close`, with each connection a
*direct descriptor* in an `io_uring` file table ( so an idle keep-alive connection costs a ~48 byte struct and
no file descriptor ). That thread must never wait on Postgres.

- **The ring thread plans, the database threads answer.** The batch handler parses the route, verifies the
  token, reads the JSON and parses the id, then puts a self-contained `PlannedRequest` on a lane queue and
  marks the response deferred. It returns immediately; the ring keeps accepting and reading while the
  database threads work.
- **Two read lanes and one write lane.** A worker starts `DB_THREADS` database threads, each with its own
  blocking libpq connection, prepared statements, response buffer and gzip scratch. The default is three ( four
  at four workers or fewer, see below ): all but the last are read lanes, and the last is reserved for
  `POST /posts` and `POST /posts/:id/like`, so a commit never delays a read. `DB_THREADS` is still a knob.
- **One worker per CPU.** The worker count follows the CPU affinity: 4, 8 and 16 workers on 4, 8 and 16
  cores, so 12, 16 and 32 read lanes. A ring's direct-descriptor table holds up to 1,048,576 connections ( the
  kernel's own limit ), so four workers still have ~4.2M slots for a 1.4M-user 4-core run. Until AmmarServer
  raised that table from 262,144 slots, the floor here had to be eight workers.
- **Responses come back on the ring thread.** A database thread builds the body ( and gzips it ), puts a
  completion on the queue and wakes the worker's eventfd once per completed batch, not once per response. The
  ring thread drains completions and sends each through `AmmServer_APISendDeferred`; a response that finds
  every send buffer busy stays queued and the fd is written again.
- **Batches within a lane.** The ring appends a pass's plans for each queue under one lock and one wake-up,
  so the waiting lane gets the whole pass instead of waking after the first plan. A lane pops up to 64
  plans, waits up to 5 ms for company when the batch is short, and sends all of their statements in one
  libpq pipeline with one Sync: one round trip and one implicit transaction. Read lanes share the transaction ( each statement still takes its own READ
  COMMITTED snapshot ); the write lane shares that commit. A quiet lane sends one.
- **Read lanes keep up to three batches in flight.** The rig re-score of the one-batch-at-a-time lane showed
  each read lane topping out near 4,200 reads/s with the Postgres box at ~23%: the lane sent a batch, waited for
  every result and built every response before sending the next, so its backend sat idle for the round trip
  and the response building. Once arrivals passed that rate the read queue filled and answered 500 (the
  rig's read-only errors). A read lane now stays in pipeline mode and, whenever at least `DB_BATCH_MIN` plans
  are waiting, puts another batch behind the one on the wire, each with its own Sync, up to three. Postgres
  runs the next batch while the lane answers the previous one. Below that backlog a lane still runs one batch
  at a time. The write lane is unchanged: one transaction per batch, and its rollback replay.
- **`SO_REUSEPORT` and prefork.** `main()` forks `WORKERS` processes and waits on them. Half listen on
  `PORT`, half on `EXTRA_PORT`; each owns its connections for their whole life. An idle connection costs one
  slot in the ring's table, not a buffer or a thread.
- **Bounded queues.** The plan queues ( 32,768 plans ) and the completion queue ( 65,536 ) are fixed-size. A
  full plan queue answers 500 instead of blocking the ring thread, because blocking there would also stop it
  draining completions. When a pass does not fit, the part that fits is queued and only the overflow is
  answered 500. With a worker per CPU a 4-core worker sees ~10k reads/s near 1.3M users, and its read queue
  holds ~3 s of them, well past the 1 s p99 limit, so only a real stall reaches this. That costs ~72 MB per
  worker of the box's 32 GB.
- **The epoll fallback answers synchronously.** When the kernel cannot set up an io_uring ring, AmmarServer
  serves API mode from its epoll transport instead. That transport has no deferred responses, so the handler
  runs the request's plan on a database lane and waits for that one completion before returning; a response
  is complete by the time epoll sends it. This is the old one-round-trip-per-worker shape rather than the
  batched lanes, but correctness does not depend on io_uring being available. The GitHub Actions runner used
  for CI cannot create io_uring rings ( `Cannot allocate memory` ), so its test run exercises this path.

Everything a worker touches is its own: its ring, its database threads, their connections, prepared
statements, buffers and HMAC key schedule. The only shared state inside a worker is the plan and completion
queues between its ring thread and its database threads, one mutex each.

## Optimizations, and why

**Prepared once, binary in and out.** The five statements are the spec's SQL verbatim, `PQprepare`d on each
database connection. Parameters go over in binary ( an `int8` is 8 bytes; the post body is its own bytes
with a length, so it never needs NUL terminating ), and results come back in binary: ids and counts are
big-endian integers, and `created_at` is microseconds since 2000-01-01 UTC. That is formatted in C, so there
is no text timestamp to parse and nothing depends on the session's `TimeZone` or `DateStyle`.

**Timestamps written by hand.** A feed formats 20 of them. Instead of `gmtime_r` and `snprintf`, the date
comes from Howard Hinnant's `civil_from_days` and the digits are written straight into the buffer. Checked
against the `gmtime_r` version over 2.18 million values from 1900 to 2400, including every day boundary:
identical output for all of them. Each database thread remembers the last day it converted, so the
calendar arithmetic only runs when the day changes; the time of day goes out two digits at a time from a
100-entry table.

**A post written in one pass.** `PutPostObject` works out the worst case a post can reach ( every body and
author byte escaped to `\u00XX` ) and, when that fits, writes the whole object with no further bounds checks:
literal pieces, ids and like counts through a two-digits-at-a-time decimal writer that counts the digits
first and fills them in place, and the body through `AmmServer_JSONEscape`. Anything that might not fit
falls back to the old piece-by-piece writers. The decimal writer matched `%llu` over 5 million values, and
whole responses were compared byte for byte with the previous build ( see Verification ).

**The ring sleeps less while it is busy.** After a pass that found completions, the ring thread waits for up
to 32 more or 1 ms in one `io_uring_enter` instead of returning to the kernel for each completion; when a pass
finds nothing it goes back to waiting without a timeout. At 200k connections on 4 vCPUs, on top of the
formatting changes, that took the server from 36.0 to 28.8 µs per request and from 1.67 to 0.66 context
switches per request, and Postgres from 4.6 to 4.2 cores, because the lanes' batches grow ( plans per
transaction 3.3 → 6.1 ). Of 100, 500 and 1000 µs, 1 ms was best, and 100 µs was worse than not waiting at
all. An idle server still uses no CPU, and at light load ( 50k users ) it is 17 % less CPU per request while
p99 moves from 5.4 to 6.9 ms.

**One Sync per lane batch, and a grouped write commit.** All statements of a lane batch are sent in one
pipeline terminated by one `Sync`. That is one round trip and one implicit transaction: a read lane shares
one transaction ( each statement still gets its own READ COMMITTED snapshot ), and a write lane commits the
whole batch with one fsync. Every
response is still held until that commit's result is in ( rule 8 ), and rule 3 allows batching writes into
one transaction. If the write transaction aborts, every write in the batch is replayed one at a time, so a
like on a missing post returns 404 without rolling back the valid posts that shared its batch; a connection
lost around the commit is answered 500 and never replayed, so a write cannot be duplicated.

**Compiler flags.** AmmarServer is built with the same `-O3 -march=native -mtune=native -flto=auto` as the
API, and the final link optimises across both, so AmmarServer's small per-field response writers can be
inlined into the handlers. Everything is built from source on the machine that runs it, which is what makes
`-march=native` safe. The hardening flags AmmarServer sets ( `-fstack-protector`, `_FORTIFY_SOURCE=2` ) are
kept: this server faces the internet.

**gzip with ISA-L igzip, level 0, feed only.** `mload -gzip` sends `Accept-Encoding: gzip` on every request,
and the spec says the network is part of the test. Level 0 is one hash lookup per position and no lazy
matching; the responses leave as two TCP segments either way. On the seed's feed a response goes from
~5.1 KB to ~2.2 KB. A single post ( ~300 bytes ) barely shrinks, so only bodies of 1 KB or more get
compressed. Compression is stateless into the database thread's own scratch buffer, so no per-request
compression state is allocated.

**One `send()` per response, JSON written straight into the response buffer.** This is unchanged from the
$12 server. AmmarServer writes the headers backwards into a reserve in front of the body, and the handlers
write JSON through bounds-checked writers with no intermediate document. The finished body is copied into an
exact-size completion for the ring thread, and from there once into the transport's send buffer.

**The HS256 key schedule is prepared once per worker.** The two SHA-256 states after `key^ipad` and
`key^opad` depend only on the secret, so they are copied per token instead of recomputed: 1.48 µs a token
down to 0.25 µs. Every signature is still computed over the bytes that arrived, and `exp` is still checked
( rules 7 and 9 ).

## Where the time goes

`profile.sh [requests] [connections]` builds a `-g` copy and runs one worker under valgrind's callgrind with
20,000 requests of the mixed load ( feeds, posts, likes, new posts, gzip ), then opens kcachegrind. Valgrind
cannot drive io_uring ( it holds its lock across the blocking `io_uring_enter`, so the lanes never run ), so
that copy is forced onto the epoll transport, and callgrind sees user space only. It guided the formatting
work above: user-space instructions for 20,000 requests went from 1,092.7M to 945.0M ( −13.5 % ). What is
left there is libpq's result parsing ( ~29 % inclusive ) and ISA-L ( ~30 % ), neither of which is this
code's.

The kernel side was measured with `perf record -g` on the io_uring build under load ( 4 workers / 3, 200k
connections on loopback ). About 70 % of the server's CPU is in the kernel, and most of that is the TCP send
of each response ( `io_send` → `tcp_sendmsg`, ~40 % ); over loopback that includes the client's receive
processing, so the rig's share is smaller. The database threads' libpq syscalls ( `poll`, `recvfrom`,
`sendto`, `futex` ) are ~18 %, receiving ~11 %. One response is already one send, as in the fastest C entry.
On the test machine's kernel ( Ubuntu 6.8.0-147 ) `IORING_REGISTER_PBUF_RING` is refused with `EINVAL` in every
form, so AmmarServer uses its classic provided-buffer fallback; that costs 0.55 % ( `io_provide_buffers` ),
so it was left alone.

## What AmmarServer gained for this

The commit pinned in `build.sh` carries all of these.

- **`AmmServer_StartAPIOnRingsReusingPort`**: `AmmServer_StartAPIOnRings` with `SO_REUSEPORT` set on the
  listening socket before it is bound, so every worker binds its own socket on the same port.
- **`AmmServer_APIResponse.extraHeaders`**: header lines a handler adds to its response, here
  `Content-Encoding: gzip\r\n`. `AmmServer_APIResponseReset` clears it.
- **`AmmServer_SetAPIBatchHandler`**: answers everything ready in one pass over the ring with one handler
  call, and keeps each request in its connection's own buffer until it is answered.
- **`AmmServer_APIResponse.deferred` / `deferToken`, `AmmServer_SetAPINotifyFD`,
  `AmmServer_APISendDeferred`**: a handler can return without filling the response, and a later callback on
  the ring thread can send it. The transport polls a non-blocking eventfd with an `io_uring` poll, keeps the
  connection's pipelined bytes until the answer's send completes, and drops answers for connections that
  have gone.
- **Connection tables of 1,048,576 slots** ( `URING_MAX_SLOTS` , was 262,144 ), the kernel's own ceiling for
  a registered file table, which is what lets a worker per CPU hold a 4-core run. A listen backlog of 65,535
  ( was 10,000 ; the kernel caps it at `net.core.somaxconn` ).
- **Batched ring waits while busy**: after a pass that found completions the ring waits for up to 32 more
  or 1 ms, instead of returning to the kernel for each one ( ~0.66 context switches/request, was ~1.6 ).
- **Cheaper response framing**: the API header is assembled from pieces instead of `snprintf`, and
  `AmmServer_JSONEscape` copies clean runs found 16 bytes at a time ( SSE2, or 8 with SWAR ) instead of one
  byte at a time.

## Verification

This revision ( a worker per CPU with two read lanes, the larger queues, and the AmmarServer changes listed
above ), locally against a fresh seed:

- `test/run.sh`: **42/42** with the new defaults on io_uring, and **42/42** with io_uring forced to fail at
  `io_uring_setup` ( the epoll fallback ), built by `build.sh` from the pinned commit. Also as CI and the rig
  build it: against Ubuntu 24.04's own `libpq-dev` 16.15 and `libisal-dev` 2.31, on a 4-CPU mask ( 4 workers
  x 4 database threads ), 42/42 on both transports.
- The mixed stress ( below ) passes alongside a 200k-connection load run of the new defaults, and a checker
  running at the same time saw every `GET /posts/:id` answer its own id and every feed hold 20 posts newest
  first: 0 mismatches over 102,509 reads with the 4-core default ( 4 x 4, that libpq 16 build ) and over
  106,241 with 4 x 3.
- One worker held **349,952 connections** ( past the old 262,144-slot table ), opening at a steady ~0.5 s
  per 25,000, and connections probed at every tenth of that range all answered `/health`.
- Responses byte for byte: 20,202 bodies over the mixed load identical to the previous build's, and 44
  complete raw responses ( status line and every header: 200, 400, 401, 404, `HEAD`, `Connection: close`,
  HTTP/1.0 ) identical.
- `AmmServer_JSONEscape` against the old byte-by-byte escape: 3 million random cases with SSE2, 3 million
  with SSE2 compiled out ( the portable 8-byte path ), and 500,000 of those under AddressSanitizer and
  UBSan, all identical and none writing past the output size. The header builder matched `snprintf` over
  2 million cases, including the oversized ones it has to refuse.

The previous revision, as checked before its rig run, with the same 42-check suite:

- `test/test.sh`: **42/42** with the dynamic defaults on 4-, 8- and 16-CPU affinity masks, and with
  `WORKERS=4 DB_THREADS=2`.
- The same **42/42** with io_uring forced to fail at `io_uring_setup` ( the epoll fallback that CI actually
  runs on ), with dynamic defaults on 2- and 4-CPU affinity masks and with `WORKERS=4 DB_THREADS=2`.
- The mixed stress below ( 200 keep-alive connections, 30 create + like + read-back sequences, 100
  create/missing-like/valid-like/read-back sequences, 200 mid-request disconnects, 20 pipelined pairs, gzip )
  on the dynamic 8-worker default : every status and count correct.
- 300 concurrent sequences of create post, like a missing post, like the new post, read it back. This
  deliberately puts failing likes in the same write transactions as valid creates; every create returned
  201, every missing-post like 404, every valid like 201, and every read-back like_count was 1. The retry
  path was observed taking over 11 rolled-back batches of up to 32 writes.
- 100 sequential create-then-`GET /feed` checks: the new post was in the feed every time, so the shared read
  transaction does not hide a commit that happened before the read request arrived.
- gzip: 5,102 bytes of feed decoded to the same JSON from a 2,160-byte response.
- `/health` answers 200 on both ports, 503 while Postgres is stopped, and recovers after it starts again.

### Local load figures

These are local proxy runs, not the rig: a desktop, the server pinned to 4 vCPUs, PostgreSQL 18 pinned to
8 vCPUs, and `mload` on 4, at 50k users with `-think-scale 0.1 -gzip`. Each run had a 30 s ramp and a 60 s
hold; the table uses the steady hold only. The local host is shared with desktop processes, so compare rows
within this table rather than the absolute p95. The 50k-connection ramp is also why the earlier small-ramp
runs showed a burst of 500s; the rig's 60 s ramp is gentler, and the hold is what the score is about.

| workers / DB threads | connections | req/s | p95 max | p99 max | errors | server CPU | CPU/request | ctx/request |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 32 / 4 (old default) | 128 | 30.5k | 1550 ms | 1566 ms | 0 | 2.53 cores | 83.0 µs | 2.47 |
| 4 / 2 | 8 | 49.0k | 164 ms | 210 ms | 0.019% | 1.52 cores | **31.0 µs** | 0.92 |
| 4 / 3 | 12 | 51.9k | 60 ms | 76 ms | 0 | 2.06 cores | 39.7 µs | 1.25 |
| 4 / 4 | 16 | 51.2k | 95 ms | 120 ms | 0 | 2.20 cores | 43.1 µs | 1.26 |
| 6 / 2 | 12 | 52.0k | 53 ms | 65 ms | 0 | 1.94 cores | 37.3 µs | 1.16 |
| 8 / 2, before batched eventfd | 16 | 51.9k | 71 ms | 96 ms | 0 | 2.18 cores | 42.0 µs | 1.37 |
| **8 / 2 dynamic default (3 runs)** | **16** | **51.8–52.1k** | **57–154 ms** | **68–186 ms** | **0** | **2.12–2.17 cores** | **40.8–41.8 µs** | **1.28–1.36** |
| 8 / 3 | 24 | 47.5k | 264 ms | 294 ms | 0 | 2.40 cores | 50.7 µs | 1.47 |
| 8 / 4 | 32 | 40.6k | 679 ms | 742 ms | 0 | 2.32 cores | 57.2 µs | 1.69 |

The first row is the old default on the same partition: 83 µs of server CPU per request and only ~30.5k
req/s. The new default holds ~51.8–52.1k req/s at ~41 µs/request, with the CPU per request cut by half.
Four workers with two database threads are cheapest (31 µs), but four ring tables cap at ~1.05M connections
and only give four read backends; six workers give six. Eight workers/2 threads gives eight read backends and
~2.1M slots, and eight workers/3 or 4 threads costs CPU and tail for no throughput. The one interesting
alternative is 4/3: it also has eight read backends, four ring threads and measured 39.7 µs at p95 60 ms,
but its four ring tables cap it at ~1.05M unless AmmarServer's slot limit is raised. Since 8/2 already fits
1.4M with more connection margin, it stays the default.

The target request rate is 43.6k req/s (1.42M on 8 cores, 1.37M on 4). At 41 µs/request that is ~1.8 cores
of the 4-vCPU server; the local partition held 51.8k req/s, and the 4-core p95 was 57–154 ms depending on
host noise. Eight workers × 262,144 ring slots is ~2.1M connections, so a 1.4M run leaves ~175k per worker.
The batched eventfd changed the before/after rows only slightly (42.0 to 41.6 µs, 1.37 to 1.31 context
switches/request), and the one-push-per-pass and 64-plan cap were flat locally (41.3 to 41.1 µs); both are
included because they are free and add headroom when the rig's Postgres round trip is slower. Extra read
lanes (8/3) were worse locally, so the default stays one read lane per worker. A five-minute hold at 50k users on the same 4-vCPU partition held
**52.1k req/s at p95 66.8 ms / p99 86.5 ms, with 0 errors in 14.6M requests and 40.8 µs CPU/request**. Above ~55k local
connections the loopback client itself runs out of ephemeral ports (`cannot assign requested address`), so
60k-user local runs are not meaningful.

The 8- and 16-core runs give the same request rate with more cores, and the local 4-vCPU partition is the
harder case. This remains a projection: the rig's 60 s ramp, 1M sockets and remote Postgres are the only
real test.

#### Pipelined read lanes, against the rig's re-score

The rig re-score of the one-batch-at-a-time lanes stopped on read-only http 500s, at about the same per-lane
rate on 8 and 4 cores ( ~4,200 reads/s per lane ) while the Postgres box sat at ~23%. A loopback Postgres
answers too quickly to show that, so the comparison below puts a small userspace TCP proxy between the server
and Postgres that holds every chunk for 5 ms in each direction. That is far more than the rig's round trip;
its purpose is to make the lane's wait, not Postgres's work, the limit, as on the rig. Same partition, 50k
users, 8 workers / 2 database threads, Postgres on 6 vCPUs, steady window:

| lane | req/s | p95 | p99 | errors | CPU/request | Postgres |
|---|---:|---:|---:|---:|---:|---:|
| one batch at a time | 34.6k | 633 ms | 659 ms | 0 | 31.5 µs | 2.7 cores |
| up to three batches in flight | **50.3k** | **196 ms** | **253 ms** | 0 | 33.5 µs | 4.3 cores |

The one-batch lane stops at ~4.0k reads/s per lane, close to the rig's figure, and the pipelined lane keeps
up with the offered load. Without the proxy the two are the same: 50k users gave 51.1–51.6k against
50.4–52.0k req/s at 41.6–41.8 against 41.5–42.2 µs/request, and 200k connections gave 41.8k against 41.9k
req/s, p95 214 against 216 ms, both with 0 errors. A checker running alongside the pipelined run under the
proxy saw every `GET /posts/:id` answer its own id ( 8,628 ) and every feed hold 20 posts newest first
( 8,548 ).

#### One worker per CPU, two read lanes each

The pipelined lanes scored 1,000,000 ( max ) on 16 cores, 1,210,000 on 8 and 1,190,000 on 4. The 8-core
hold that passed still had 5,222 http 500s, and a 1,270,000 hold failed on p99 ( 1.06 s ) with 15,762 of
them: the same read-lane ceiling as before, only higher, since 8 cores still meant 8 workers with one read
lane each. The fastest C entry runs one thread per CPU with ~64 Postgres connections; its socket setup
( `SO_REUSEPORT` on both ports, `TCP_NODELAY` inherited from the listener, the descriptor limit raised )
was already the same as here, apart from its 65,535 listen backlog, which AmmarServer now matches.

With the ring table raised to 1,048,576 slots the eight-worker floor is no longer needed, and the 4 / 3 shape
above ( one ring thread per CPU, two read lanes each ) became the default; on 4 cores it then gets a third
read lane ( below ). Per CPU this is the 8- and 16-core shape. Same 4-vCPU partition,
`mload` on 4 others, two interleaved runs of each, steady window. Through the 5 ms proxy, 50k users, Postgres
on 6 vCPUs:

| workers / DB threads | read lanes | req/s | p95 | p99 | CPU/request | ctx/request | Postgres |
|---|---:|---:|---:|---:|---:|---:|---:|
| 8 / 2 ( old default ) | 8 | 51.1k | 66–75 ms | 82–99 ms | 26.2–26.4 µs | 0.35 | 4.3 cores |
| 4 / 2 | 4 | 49.9–50.0k ( behind ) | 140–147 ms | 181–234 ms | 23.8 µs | 0.26 | 3.7 cores |
| **4 / 3 ( the 8- and 16-core default, per CPU )** | **8** | **51.3k** | **60–61 ms** | **78–79 ms** | **25.0–25.1 µs** | **0.25** | **4.2 cores** |
| 4 / 4 | 12 | 51.3k | 64 ms | 79–82 ms | 25.7–25.9 µs | 0.26 | 4.5 cores |
| 8 / 3 | 16 | 51.0k | 73–76 ms | 87–89 ms | 27.9 µs | 0.35 | 4.8 cores |

Without the proxy, 200k connections over 8 loopback addresses, Postgres on 6 vCPUs:

| workers / DB threads | req/s | p95 | p99 | CPU/request | ctx/request | Postgres |
|---|---:|---:|---:|---:|---:|---:|
| 8 / 2 ( old default ) | 42.0k | 151–159 ms | 251 ms | 28.4–28.5 µs | 0.66 | 3.8–3.9 cores |
| **4 / 3 ( the 8- and 16-core default, per CPU )** | **42.1k** | **148–153 ms** | **234–244 ms** | **25.8–25.9 µs** | **0.50** | **3.6–3.7 cores** |
| 4 / 4 | 41.7–42.0k | 190–192 ms | 261–272 ms | 27.2–27.6 µs | 0.51 | 4.1 cores |

4 / 2 is cheapest without the proxy but falls behind with it: four read lanes are not enough once the round
trip is the limit. On 8 and 16 cores the same shape doubles the read lanes from 8 and 16 to 16 and 32. Two
other shapes were measured and dropped: the ring thread driving its own non-blocking libpq connections ( 4
workers with 8 or 16 each, no database threads ) cut context switches to 0.075/request but cost ~27 % more
Postgres CPU through the 5 ms proxy for no better latency.

On 4 cores a worker gets a third read lane. A 15 ms delay in front of Postgres caps each lane by its
waiting, which is how the rig's per-lane ceiling behaves; there 4 / 3 fell behind and 4 / 4 kept up. On
loopback, where lanes are not the limit, the third lane costs CPU and Postgres work ( 50k users through the
proxy, 200k connections on loopback, two runs each, steady window ):

| 4 cores | through a 15 ms proxy: req/s | p95 | p99 | loopback: CPU/request | Postgres | p95 | p99 |
|---|---:|---:|---:|---:|---:|---:|---:|
| 4 workers / 3 DB threads ( 8 read lanes ) | 46.2k ( behind ) | 179–184 ms | 200–232 ms | 25.8–26.3 µs | 3.7 cores | 157 ms | 241–246 ms |
| **4 workers / 4 DB threads ( 12 read lanes ), the 4-core default** | **50.0k** | **95–97 ms** | **112–118 ms** | 28.0–28.9 µs | 4.2 cores | 179–202 ms | 264–286 ms |

#### Theoretical peak workload ( derived, not measured )

The load model's user repeats a feed, one post from it, a like 15 % of the time and a new post 2 % of the
time, with 51.25 s of think and idle time per cycle at `-think-scale 2.5`. That is **42.3k req/s per million
users in the steady hold**, 92 % of them reads ( 39.0k reads/s and 3.3k writes/s ). Each limit below is
turned into users with that rate, separately for each core count:

| limit | 4 cores | 8 cores | 16 cores | basis |
|---|---:|---:|---:|---|
| connections the rings can hold | 4.19M | 8.39M | 16.8M | 1,048,576 slots × workers, each ring within the rig's `LimitNOFILE` of 8,388,608 |
| Postgres connections used | 16 | 24 | 48 | of the 512 allowed |
| read lanes | 12 | 16 | 32 | three per worker on 4 cores, two on 8 and 16 |
| read-lane ceiling | ~1.8M | **~2.4M** | ~4.8M | ~5.85k reads/s per lane, where #84 stopped on both 8 ( 1.21M ) and 4 cores ( 1.19M ) |
| CPU at 70 % ( where p99 usually still holds ) | **~1.45M** | ~3.2M | ~6.3M | ~46 µs/request on 4 cores and ~42 on 8 and 16, on the rig, extrapolated |
| CPU at 100 % | ~2.05M | ~4.5M | ~9.0M | same |

The read-lane rate is the one number taken from the rig. #84 stopped near 5.8–5.9k reads/s per lane on both
8 and 4 cores, at very different CPU, so it behaves like a property of one Postgres connection's statement
stream, and nothing here changes how fast Postgres runs a statement. The CPU figure is #80's measured rig
cost ( ~63 µs/request on 4 cores ), +5 % for pipelining, scaled by the local ratio between #84 and this
revision at 200k connections ( 40.6 → 25.9 µs, and ~9 % more for the third read lane on 4 cores ). It is an
extrapolation and could easily be ±15 %. The write lane is far from its limit: at 1.4M users on 4 cores each
one sees ~1.2k writes/s, committed up to 64 to a transaction.

So, server side: **4 cores becomes CPU bound at roughly 1.45M** ( with two read lanes per worker it would
have stayed read-lane bound at ~1.2M, where #84 already was, which is why it gets a third ); **8 cores moves
from ~1.2M to a ~2.4M lane ceiling**; 16 cores is scored up to 1,000,000 ( max ) and is far from every limit
here. Past these server-side numbers is the shared Postgres box: the best 8-core score
any entry has reached is 1.45M, and that, not this table, is the likely ceiling on 8 cores.

## Rule notes

- **Rule 3**: one query per request, the spec's SQL as written. Queries are prepared and pipelined, which
  the rule allows. Nothing is moved into Postgres. JSON, timestamps and gzip are all done in C.
- **Rule 5**: `WORKERS * DB_THREADS` connections, capped at 512. The defaults are 4 * 4 = **16**, 8 * 3 =
  **24** and 16 * 3 = **48** on 4-, 8- and 16-core runs.
- **Rule 7, no caching**: every response comes from a query run while serving it. Prepared statements,
  the connections and the transport's buffers are the only things reused between requests. ( The timestamp
  writer remembers the calendar date of the last day number it converted; that is arithmetic on the value
  in hand, not stored data. )
- **Rule 8, committed before responding**: a write lane's batch is one transaction ended by one `Sync`; no
  response in it is queued until that Sync's result has been read, which comes after the commit. A rolled
  back batch is replayed one write at a time, each its own transaction, so every response still follows its
  own commit. Rule 3 explicitly allows batching several requests' writes into one transaction.
- **Rule 10, crashes**: a worker that dies is reported and not restarted. The others keep serving their own
  connections. Every worker has `PR_SET_PDEATHSIG`, so none outlives the parent.
- **Rule 13, nothing tuned on the box**: the only setting changed is this process's own `RLIMIT_NOFILE` soft
  limit, raised to its hard limit. Under the benchmark's `LimitNOFILE=2097152` the two are already equal.

## Files

| | |
|---|---|
| `feedapi.c` | the prefork parent, the five endpoints, routing, planning, the database lanes, completions and gzip |
| `jwt.c/.h` | HS256 verification: base64url, the distinct 401s the spec asks for, `CRYPTO_memcmp` |
| `install.sh` | apt: `build-essential cmake git libssl-dev libpq-dev libisal-dev ca-certificates` |
| `build.sh` | fetches AmmarServer at the pinned commit, builds its library target, links `build/feedapi` |
| `start.sh` | `exec build/feedapi` in the foreground |
| `profile.sh` | callgrind profile of one worker under 20,000 requests ( see Where the time goes ) |
| `debug.sh` | builds the rootless local test environment under `/tmp/muc-setup` ( Postgres 18, ISA-L, mload, the io_uring-failing shim ); not used by the benchmark |

`WORKERS=N` overrides the worker count ( default the affinity CPU count, at least one per port, at
most 512 ). `DB_THREADS=N` overrides the database threads per worker ( default 3, or 4 at 4 workers or fewer, reduced automatically when
512 / `WORKERS` is smaller ). `DB_BATCH_MIN` and `DB_BATCH_US` tune a lane's coalescing window ( default 8
plans and 5,000 µs ; 0 disables the wait ) so it can be swept from `start.sh` without a rebuild. `build.sh`
honours `AMMARSERVER_SRC=/path/to/a/checkout` to build against a local AmmarServer tree instead of fetching
the pinned commit; that tree has to be at least the pinned commit ( it carries the deferred-response API ).
CI and the benchmark box set neither.
