# C + AmmarServer, one thread per CPU speaking the Postgres wire protocol itself, served direct on :80 and :81

| | |
|---|---|
| **Language** | C ( C11, gcc 13, `-O3 -march=native -flto`, for AmmarServer as well as the API ) |
| **HTTP server** | [AmmarServer](https://github.com/AmmarkoV/AmmarServer) in API mode on `io_uring`, pinned by commit in `build.sh` |
| **Postgres driver** | libpq 16 ( Ubuntu 24.04's `libpq-dev` ) connects, authenticates and prepares the statements; after that each worker speaks the v3 wire protocol on the connection's socket itself ( Bind + Execute per request, one Sync per group, no Describe, rows read in place ) |
| **Compression** | ISA-L 2.31 igzip ( Ubuntu 24.04's `libisal-dev` ), level 0, feed responses only, with its Huffman table rebuilt about once a second from a live response |
| **Crypto** | OpenSSL `libcrypto` for HS256 |
| **Processes** | 1 parent + `WORKERS` worker processes, half on `PORT` and half on `EXTRA_PORT`; each worker is one `io_uring` ring thread that also drives that worker's Postgres connections |
| **Postgres connections** | `WORKERS * DB_CONNECTIONS`, **64 in all** by default ( **16**, **8** and **4** per worker on 4-, 8- and 16-core runs ), a quarter of a worker's for writes; at most one more per worker, used only by the synchronous fallback. Rule 5's **512** is never reached. |

This is my [$12 server challenge](https://github.com/arjaythedev/twelve-dollar-server-challenge) submission
moved onto 16 cores and Postgres. The HTTP side, routing, JSON writing and HS256 verification are the same code.
The entry scored in #92 kept database threads behind each ring thread. This revision has none: the ring thread
plans each request, writes the pass's statements to Postgres itself and answers when their results arrive.
AmmarServer's ring paces a busy pass, so everything that arrived in the last 2 ms is served as one batch.

Two steps, both measured locally on the same runs ( 4 server vCPUs, 50k users at ~52k req/s ):

| build | CPU / request | context switches / request | TCP segments / request | feed, gzipped |
|---|---:|---:|---:|---:|
| #92's code ( database threads, libpq ) | 31.0 µs | 0.451 | 3.625 | 2,160 B |
| ring thread driving libpq | 25.7 µs | 0.026 | 3.571 | 2,160 B |
| **this revision ( ring thread, own wire code )** | **22.6 µs** | **0.027** | **3.534** | **1,961 B** |
| `c-epoll-unorig-username`, the fastest C entry | 23.0 µs | 0.037 | 3.548 | — |

The rig is the only test that matters and has not run this revision; every number in this README is local.

## Design

AmmarServer's API mode is one thread owning every connection from `accept` to `close`, with each connection a
*direct descriptor* in an `io_uring` file table ( so an idle keep-alive connection costs a ~48 byte struct and
no file descriptor ). Everything below happens on that one thread.

- **The ring plans and drives Postgres.** The batch handler parses the route, verifies the token, reads the
  JSON and the id, then appends each database-bound request to the output of one of the worker's
  non-blocking Postgres connections. The connections live in an epoll set whose descriptor the ring polls through
  `AmmServer_SetAPINotifyFD`; results are read and answered from that same callback. The request path has no
  other thread, no queue and no lock.
- **A pass is a batch.** While the ring is busy it waits for up to 256 completions or 2 ms in one
  `io_uring_enter` before serving what arrived ( `URING_BATCH_PACED`, set in `build.sh` ). At 42,000 req/s on
  four cores that is ~20 requests per worker per 2 ms pass. A pass's reads go to one read connection as one
  run of Bind/Execute ended by one `Sync`, and its writes go to one write connection as one transaction ended
  by the same kind of `Sync`. One wake-up, one write to Postgres and one round trip serve the batch. The cost
  is up to 2 ms of latency, against the 500 ms p95 limit.
- **A group spans the whole pass.** AmmarServer hands the handler at most 32 requests a call, and at
  1.5M users on four cores a 2 ms pass holds about that many, so a pass is often two calls. The group stays
  open across them and gets its `Sync` when a call arrives that is not full ( or at 128 statements ), so a
  busy pass is still one write and one round trip rather than one per call.
- **Least-in-flight connections.** A worker opens `DB_CONNECTIONS` connections ( 16 on a 4-core run, 8 on 8,
  4 on 16 ), a quarter of them writers ( at least one ). Each group goes to the connection of its kind with
  the fewest statements in flight. When Postgres answers faster than the 2 ms pace, one connection carries
  everything; when a round trip is slow or a packet is lost, the next pass goes to another connection, so the
  worker keeps going without waiting. With the rig's slower round trip a 4-core worker spreads its passes over
  its 12 read connections.
- **Bounded in flight.** Each connection's flight ring holds 2,048 statements and Syncs, about 3 s of a
  worker's reads. A pass that finds every connection of a kind full answers its share with 500 instead of
  queueing without bound; at a million users that ring is well past the 1 s p99 limit.
- **Answers leave when their results are in.** A read's answer is built from its DataRows where they lie in
  the connection's receive buffer and sent the moment its `CommandComplete` arrives. A write's answer waits
  for the `ReadyForQuery` of the `Sync` that commits its group ( rule 8 ). A response that finds all
  256 send tickets of the transport busy waits in a FIFO, and nothing more is read from Postgres until it
  has gone out, so a slow client cannot grow an unbounded queue. The send buffers cost ~48 MB per worker;
  the flight rings ~2.25 MB.
- **The epoll fallback answers synchronously.** When the kernel cannot set up an `io_uring` ring, AmmarServer
  serves API mode from its epoll transport instead. That transport has no deferred responses, so the handler
  runs one statement through libpq on a blocking connection of its own ( opened lazily, retried at most once
  a second ) and answers before returning. Correctness does not depend on `io_uring`; the GitHub Actions runner used for CI
  cannot create rings, so its test run exercises this path.
- **`SO_REUSEPORT` and prefork.** `main()` forks `WORKERS` processes and waits on them. Half listen on
  `PORT`, half on `EXTRA_PORT`; each owns its connections and its Postgres connections for their whole life.
  A ring's table holds up to 1,048,576 connections, so four workers still have ~4.2M slots for a 1.4M-user
  4-core run. An idle connection costs one table slot, not a buffer or a thread.

## Optimizations, and why

**Its own wire code after login.** libpq opens each connection, authenticates ( whatever the server asks
for, SCRAM included ) and `PQprepare`s the five statements, which are the spec's SQL verbatim. From then on
libpq only owns the socket, and the worker writes and reads it directly. A request is one `Bind` to the
prepared statement plus one `Execute`, ~40 bytes for a read; a group ends with one `Sync`.
`PQsendQueryPrepared` always adds a `Describe`, so with libpq every reply carried a RowDescription
( ~130 bytes ) that Postgres had to build and nobody read; now a reply is `BindComplete`, the DataRows and
`CommandComplete`. Those are parsed where they sit in the connection's 256 KB receive buffer, with no
`PGresult` and so no allocation or copy per row; libpq's result handling was about 29 % of the user-space
instructions of the previous revision. After an error Postgres skips to the `Sync`, so the statements behind
it get no reply, and the `ReadyForQuery` ends the group either way. Unexpected message types drop the
connection like a lost one. This is what took CPU per request from 25.7 to 22.6 µs.

**Binary where it helps, text where it is shorter.** Parameters go over in binary ( an `int8` is 8 bytes;
the post body is its own bytes with a length ). In results `created_at` is binary, microseconds since
2000-01-01 UTC, formatted in C, so nothing depends on the session's `TimeZone` or `DateStyle`. Ids and like
counts are asked for as text: fewer bytes than an `int8`, and their digits are copied into the JSON as they
are.

**Timestamps written by hand.** A feed formats 20 of them. Instead of `gmtime_r` and `snprintf`, the date
comes from Howard Hinnant's `civil_from_days` and the digits are written straight into the buffer. Checked
against the `gmtime_r` version over 2.18 million values from 1900 to 2400, including every day boundary:
identical output for all of them. Each worker remembers the last day it converted, so the calendar arithmetic
only runs when the day changes; the time of day goes out two digits at a time from a 100-entry table.

**A post written in one pass.** `PutPostObject` works out the worst case a post can reach ( every body and
author byte escaped to `\u00XX` ) and, when that fits, writes the whole object with no further bounds checks:
literal pieces, the id's and like count's digits as Postgres sent them, and the body through
`AmmServer_JSONEscape`. Anything that might not fit falls back to the piece-by-piece writers.

**The ring paces instead of spinning.** After a pass that found work, a busy single ring waits up to 2 ms for
256 completions in one `io_uring_enter` ( AmmarServer's default is 32 or 1 ms ); an idle ring still blocks
without a timeout, so an idle server uses no CPU. In the same 50k-user run, the previous revision's lane
hand-off cost 0.451 context switches per request and 31.0 µs; this one costs 0.026 and 25.8 µs. At 200k
connections it is 0.034 against 0.515 and 27.5 µs against 30.1 µs. The pace also makes the groups: requests
per Postgres transaction rose from 9.87 to 12.05 in that run, and outbound segments per request fell from
1.255 to 1.197.

**One Sync per group, and a grouped write commit.** A read group is one run of statements terminated by one
`Sync`: one round trip and one implicit transaction ( each statement still takes its own READ COMMITTED
snapshot ), so it sees every commit made before it ran. A write group is one transaction committed by one
`Sync`; every response in it is held until that Sync is back ( rule 8 ), and rule 3 allows batching writes
into one transaction. If one write in a group fails ( a like on a missing post ), the whole group rolls back:
the failing like is answered 404, and every other write is replayed as its own transaction, so the valid
posts and likes commit. An error at the `Sync` itself ( the commit ) fails or replays the group the same way. A connection lost around a Sync is answered 500 and never replayed, so a write cannot be
duplicated.

**Compiler flags.** AmmarServer is built with the same `-O3 -march=native -mtune=native -flto=auto` as the
API, and the final link optimises across both, so AmmarServer's small per-field response writers can be
inlined into the handlers. Everything is built from source on the machine that runs it, which is what makes
`-march=native` safe. The hardening flags AmmarServer sets ( `-fstack-protector`, `_FORTIFY_SOURCE=2` ) are
kept: this server faces the internet.

**gzip with ISA-L igzip, level 0, feed only.** `mload -gzip` sends `Accept-Encoding: gzip` on every request,
and the spec says the network is part of the test. Level 0 is one hash lookup per position and no lazy
matching; the responses leave as two TCP segments either way. Level 0 does not build a Huffman code for
what it compresses; it uses a table made beforehand, by default one for general data. About once a second
each worker rebuilds that table from the symbols of the feed it is compressing
( `isal_update_histogram`, `isal_create_hufftables` ), as the F# and Go entries do. It is a code table, not
content: every response is still compressed from its own bytes, and any table encodes any input correctly.
On the seed's feed a response goes from 5,102 bytes to 1,961 ( 2,160 with the default table ); the cost is
one table build a second per worker. A single post ( ~300 bytes ) barely shrinks, so only bodies of 1 KB or more get
compressed. Compression is stateless into the worker's own scratch buffer, so no per-request compression
state is allocated.

**One `send()` per response, JSON written straight into the response buffer.** AmmarServer writes the headers
backwards into a reserve in front of the body, and the handlers write JSON through bounds-checked writers
with no intermediate document. The finished body, or its gzip straight from the compressor's buffer, is
copied once into the transport's send buffer.

**The HS256 key schedule is prepared once per worker.** The two SHA-256 states after `key^ipad` and
`key^opad` depend only on the secret, so they are copied per token instead of recomputed: 1.48 µs a token
down to 0.25 µs. Every signature is still computed over the bytes that arrived, and `exp` is still checked
( rules 7 and 9 ).

## Where the time goes

`profile.sh [requests] [connections]` builds a `-g` copy and runs one worker under valgrind's callgrind with
20,000 requests of the mixed load ( feeds, posts, likes, new posts, gzip ). Valgrind cannot drive `io_uring`
( it holds its lock across the blocking `io_uring_enter` ), so that copy is forced onto the epoll transport,
which is the synchronous libpq fallback, and callgrind sees user space only. So it profiles planning, the
token check, JSON and gzip, but not the wire code. It was not run again for this revision; on the previous
one it counted 876.4M instructions for 20,000 requests, 29.5 % of them ISA-L, about 29 % libpq's result
parsing and allocation ( the part the ring path no longer has ) and 5.4 % JSON escaping.

The kernel side was measured with `perf record -g` on the previous revision under load ( 4 workers, 200k
connections on loopback ). About 70 % of the server's CPU was in the kernel, and most of that was the TCP send
of each response ( `io_send` → `tcp_sendmsg`, ~40 % ); over loopback that includes the client's receive
processing, so the rig's share is smaller. One response is already one send, as in the fastest C entry. What
changed is where Postgres is spoken to: its `recv`/`send` are the ring thread's own syscalls instead of
another thread's, which is why context switches per request fell to 0.03, and one `recv` per wake-up takes
everything the socket holds.

On the test machine's kernel ( Ubuntu 6.8 ) `IORING_REGISTER_PBUF_RING` is refused with `EINVAL` in every
form, so AmmarServer uses its classic provided-buffer fallback; that costs 0.55 % ( `io_provide_buffers` ),
so it was left alone.

## What AmmarServer gained for this

The revision pinned in `build.sh` carries all of these.

- **Paced batched ring waits**: `URING_BATCH_PACED` sends a busy single ring into the short batched wait after
  every pass instead of looping for as long as a pass finds work, so each pass covers everything that arrived
  in `URING_BATCH_WAIT_US` ( here 2 ms ); `URING_BATCH_COMPLETIONS` ( here 256 ) is how many it waits for.
  `URING_SEND_TICKETS` ( here 256, was a fixed 32 ) sizes the pool of responses that may be in flight.
- **`TCP_THIN_LINEAR_TIMEOUTS`** on the Postgres sockets and on the listener ( accepted sockets inherit it,
  checked on this kernel ), and **`TCP_DEFER_ACCEPT`** on the listener: a client speaks first, so the
  connection is handed to `accept` when its request is already there. Both are per-socket settings, within
  rule 13.
- **`AmmServer_StartAPIOnRingsReusingPort`**: `AmmServer_StartAPIOnRings` with `SO_REUSEPORT` set on the
  listening socket before it is bound, so every worker binds its own socket on the same port.
- **`AmmServer_APIResponse.extraHeaders`**: header lines a handler adds to its response, here
  `Content-Encoding: gzip\r\n`. `AmmServer_APIResponseReset` clears it.
- **`AmmServer_SetAPIBatchHandler`**: answers everything ready in one pass over the ring with one handler
  call, and keeps each request in its connection's own buffer until it is answered.
- **`AmmServer_APIResponse.deferred` / `deferToken`, `AmmServer_SetAPINotifyFD`,
  `AmmServer_APISendDeferred`**: a handler can return without filling the response, and a later callback on
  the ring thread can send it. The transport polls the fd with an `io_uring` poll, keeps the connection's
  pipelined bytes until the answer's send completes, and drops answers for connections that have gone.
  Here that fd is an epoll descriptor covering every Postgres connection of the worker.
- **Connection tables of 1,048,576 slots** ( `URING_MAX_SLOTS` , was 262,144 ), the kernel's own ceiling for
  a registered file table, and a listen backlog of 65,535 ( was 10,000 ; the kernel caps it at
  `net.core.somaxconn` ).
- **Cheaper response framing**: the API header is assembled from pieces instead of `snprintf`, and
  `AmmServer_JSONEscape` copies clean runs found 16 bytes at a time ( SSE2, or 8 with SWAR ) instead of one
  byte at a time.

## Verification

Against a fresh copy of the seed, with the defaults:

- `test/run.sh`: **42/42** on `io_uring`, and **42/42** with `io_uring_setup` forced to fail ( the epoll
  fallback CI runs on ).
- The mixed stress: keep-alive clients cycling create, like a missing post, like the new post, like it
  again, read it back and read the feed, so failing likes land in the same write groups as valid creates and
  likes; plus 20 pipelined request pairs and 20 clients that disconnect mid-request. 16 clients x 40 cycles
  and 48 x 30 on the default build, and 200 x 12 and 400 x 6 on an AddressSanitizer + UBSan build paced at
  50 ms, so that write groups span several handler calls and reach the 128-statement limit ( 4,191 groups
  rolled back and replayed ). Every status and every like count correct, 0 failures, no sanitizer report.
- Postgres stopped and started under the running server: `/health` 200 -> 503 with "the connection to the
  database was lost" -> 200, and `/feed` 500 -> 200. The broken connections were dropped, answered and
  reopened; no write was replayed.
- A gzipped feed decompresses to the same bytes as the plain one.

**Segments and transactions.** Same 50k-user run, 30 s ramp and 90 s hold, server and load generator on
`vCPUs 0-3` and `12-15`, Postgres on `4-11`; the window is the steady 30 s and the counters are the server's
own sockets and its Postgres sockets ( `ss -tin`, `segs_out`/`segs_in` ):

| build | segments / request | out / request | requests / transaction |
|---|---:|---:|---:|
| #92's code | 3.625 | 1.255 | 9.87 |
| ring thread driving libpq | 3.571 | 1.197 | 12.05 |
| **this revision** | **3.534** | **1.190** | **12.27** |
| `c-epoll-unorig-username` | 3.548 | 1.198 | 13.64 |

Loopback's MTU here is 65,536, not the rig's 1,500, so the client-side segment counts are lower than they
would be on the rig; the comparison between the three builds is the point, and the Postgres-side packets do
not depend on that MTU.

**Load.** Server on 4 vCPUs, Postgres 18 on 8, `mload` on 4, `-gzip`, steady window, zero failed requests in
every run. 50k users (`-think-scale 0.1`, 30 s ramp, 60 s hold ):

| build | req/s | p95 / p99 | CPU / request | ctx / request | Postgres |
|---|---:|---:|---:|---:|---:|
| #92's code | 51,936 | 55 / 66 ms | 31.0 µs | 0.451 | 4.91 cores |
| ring thread driving libpq | 51,964 | 58 / 69 ms | 25.7 µs | 0.026 | 4.62 cores |
| **this revision** | **52,381** | **49 / 68 ms** | **22.6 µs** | **0.027** | **4.46 cores** |
| `c-epoll-unorig-username` | 51,982 | 57 / 69 ms | 23.0 µs | 0.037 | 4.39 cores |

200,000 connections over eight loopback addresses (`-think-scale 0.5`, 60 s ramp, 60 s hold ):

| build | req/s | p95 / p99 | CPU / request | ctx / request | Postgres |
|---|---:|---:|---:|---:|---:|
| #92's code | 41,711 | 202 / 266 ms | 30.1 µs | 0.515 | 3.85 cores |
| ring thread driving libpq | 42,034 | 192 / 261 ms | 27.5 µs | 0.034 | 3.70 cores |
| **this revision** | **42,056** | **159 / 286 ms** | **25.5 µs** | **0.035** | **3.66 cores** |
| `c-epoll-unorig-username` | 42,063 | 206 / 256 ms | 25.6 µs | 0.046 | 3.69 cores |

The local host is shared with desktop processes, so compare rows within a table, not absolute milliseconds;
the tails move by tens of milliseconds between runs of the same build. The #92 rows, the 200k libpq row
and the libpq segment row are from earlier runs on the same machine and scripts; the others were run
together for this revision. A 4 ms pace was tried against the 2 ms one ( 23.1 against 22.8 µs, Postgres 4.39 against 4.48
cores ): within noise, so 2 ms stays.

#### Theoretical peak workload ( derived, not measured )

The load model's user repeats a feed, one post from it, a like 15 % of the time and a new post 2 % of the
time, with 51.25 s of think and idle time per cycle at `-think-scale 2.5`. That is **42.3k req/s per million
users in the steady hold**, 92 % of them reads ( 39.0k reads/s and 3.3k writes/s ). Each limit below is
turned into users with that rate, separately for each core count:

| limit | 4 cores | 8 cores | 16 cores | basis |
|---|---:|---:|---:|---|
| connections the rings can hold | 4.19M | 8.39M | 16.8M | 1,048,576 slots × workers, each ring within the rig's `LimitNOFILE` of 8,388,608 |
| Postgres connections used | 68 | 72 | 80 | 64 steady + at most one synchronous per worker, of the 512 allowed |
| read connections | 48 | 48 | 48 | three quarters of the 64, whichever the core count |
| a read group | ~20 | ~10 | ~5 | the pass's reads at the target rate; a group spans the pass's handler calls, up to 128 statements |
| CPU at 70 % ( where p99 usually still holds ) | **~1.75M** | ~3.5M | ~7.0M | ~37 µs/request on the rig, extrapolated |
| CPU at 100 % | ~2.5M | ~5.1M | ~10M | same |

The CPU figure starts from #92's ~49 µs/request at its 4-core hold on the rig ( 38.5k req/s at 47 % ), scaled
by this revision's measured local ratio against that code ( 22.6 / 31.0 at 50k, 25.5 / 30.1 at 200k, so
~0.75-0.85 ). It is an extrapolation and could easily be ±15 %, and CPU is not what stops the top entries on
4 cores: they all fail near 1.55-1.6M at about half their CPU, with latency jumping to seconds. The read side is no longer the limit it was in #84: a worker's 12 read connections each take a
whole group, and only when a round trip outlasts the 2 ms pace does another one get used, so 12 × 128 per
round trip is far past the target rate. Past the server-side numbers is the shared Postgres box: the best
8-core score any entry has reached is 1.48M, and that, not this table, is the likely ceiling on 8 cores.
16 cores is scored up to 1,000,000 ( max ) and is far from every limit here.

## Rule notes

- **Rule 3**: one query per request, the spec's SQL as written. Queries are prepared and pipelined, which
  the rule allows. Writing the wire messages by hand changes what is on the wire, not what Postgres runs: the
  same prepared statements, the same parameters, one Bind/Execute per request. Nothing is moved into Postgres. JSON, timestamps and gzip are all done in C.
- **Rule 5**: `WORKERS * DB_CONNECTIONS`, 64 by default, plus at most one synchronous connection per worker
  ( opened only by the epoll fallback or a request that arrives before the batch handler is installed ):
  **68**, **72** and **80** on 4-, 8- and 16-core runs, well under 512.
- **Rule 7, no caching**: every response comes from a query run while serving it. Prepared statements,
  the connections, the transport's buffers and the gzip Huffman code table ( symbol statistics, rebuilt
  about once a second, never response content ) are the only things reused between requests. ( The timestamp
  writer remembers the calendar date of the last day number it converted; that is arithmetic on the value
  in hand, not stored data. )
- **Rule 8, committed before responding**: a write group is one transaction ended by one `Sync`; no response
  in it is built until that Sync's `ReadyForQuery` has been read, which comes after the commit. A rolled-back group is
  replayed one write at a time, each its own transaction. A connection lost around the Sync answers 500 and
  is not replayed.
- **Rule 9**: every token's signature and `exp` are checked on every request; nothing is remembered.
- **Rule 10, crashes**: a worker that dies is reported and not restarted. The others keep serving their own
  connections. Every worker has `PR_SET_PDEATHSIG`, so none outlives the parent.
- **Rule 13, nothing tuned on the box**: the only settings changed are this process's own `RLIMIT_NOFILE`
  soft limit and per-socket TCP options on its own sockets. Nothing is changed server-wide.

## Files

| | |
|---|---|
| `feedapi.c` | the prefork parent, the five endpoints, routing, planning, the Postgres wire code and connections, and the deferred answers |
| `jwt.c/.h` | HS256 verification: base64url, the distinct 401s the spec asks for, `CRYPTO_memcmp` |
| `install.sh` | apt: `build-essential cmake git libssl-dev libpq-dev libisal-dev ca-certificates` |
| `build.sh` | fetches AmmarServer at the pinned commit, builds its library target, links `build/feedapi`; the transport's batching limits are `TRANSPORT_FLAGS` |
| `start.sh` | `exec build/feedapi` in the foreground |
| `profile.sh` | callgrind profile of one worker under 20,000 requests ( see Where the time goes ) |
| `debug.sh` | builds the rootless local test environment ( Postgres 18, ISA-L, `mload`, the io_uring-failing shim ) and the pinned load scripts; not used by the benchmark |

`WORKERS=N` overrides the worker count ( default the affinity CPU count, at least one per port, at most 128 ).
`DB_CONNECTIONS=N` overrides the Postgres connections per worker ( default 64 / `WORKERS`, at least 2 and at
most 16, reduced automatically so the total stays under 512 ). `build.sh` honours
`AMMARSERVER_SRC=/path/to/a/checkout` to build against a local AmmarServer tree instead of fetching the pinned
commit, and `TRANSPORT_FLAGS` to override the batching defines. CI and the benchmark box set neither.
