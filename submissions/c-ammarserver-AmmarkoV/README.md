# C + AmmarServer + libpq, deferred responses, served direct on :80 and :81

| | |
|---|---|
| **Language** | C ( C11, gcc 13, `-O3 -march=native -flto`, for AmmarServer as well as the API ) |
| **HTTP server** | [AmmarServer](https://github.com/AmmarkoV/AmmarServer) in API mode on `io_uring`, pinned by commit in `build.sh` ( `80e68cdd` ) |
| **Postgres driver** | libpq 16 ( Ubuntu 24.04's `libpq-dev`, which speaks to Postgres 18 ), prepared statements, pipeline mode, binary results |
| **Compression** | ISA-L 2.31 igzip ( Ubuntu 24.04's `libisal-dev` ), level 0, feed responses only |
| **Crypto** | OpenSSL `libcrypto` for HS256 |
| **Processes** | 1 parent + `WORKERS` ( default **32** ) worker processes, half on `PORT` and half on `EXTRA_PORT` |
| **Database threads** | `DB_THREADS` per worker ( default **4** : read lanes plus one write lane ) |
| **Postgres connections** | `WORKERS * DB_THREADS`, capped at the **512** allowed ; **128** with the defaults |

This is my [$12 server challenge](https://github.com/arjaythedev/twelve-dollar-server-challenge) submission
moved onto 16 cores and Postgres. The HTTP side, the routing, the JSON writing and the HS256 verification are
the same code. The previous revision kept one blocking libpq connection per worker; it held a million users on
16 and 4 cores and 1.02M on 8 ( #20 on the leaderboard ), but its p95 rose before its CPU did, because the one
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
- **Read lanes and a write lane.** A worker starts `DB_THREADS` database threads, each with its own blocking
  libpq connection, prepared statements, response buffer and gzip scratch. All but the last are read lanes;
  the last is reserved for `POST /posts` and `POST /posts/:id/like`, so a commit never delays a read.
- **Responses come back on the ring thread.** A database thread builds the body ( and gzips it ), puts a
  completion on the queue and writes the worker's eventfd. The ring thread drains completions and sends each
  through `AmmServer_APISendDeferred`; a response that finds every send buffer busy stays queued and the fd
  is written again.
- **Batches within a lane.** A lane pops up to 32 plans, waits up to 5 ms for company when the batch is
  short, and sends all of their statements in one libpq pipeline with one Sync: one round trip and one
  implicit transaction. Read lanes share the transaction ( each statement still takes its own READ
  COMMITTED snapshot ); the write lane shares that commit. A quiet lane sends one.
- **`SO_REUSEPORT` and prefork.** `main()` forks `WORKERS` processes and waits on them. Half listen on
  `PORT`, half on `EXTRA_PORT`; each owns its connections for their whole life. An idle connection costs one
  slot in the ring's table, not a buffer or a thread.
- **Bounded queues.** The plan queues and the completion queue are fixed-size. A full plan queue answers 500
  instead of blocking the ring thread, because blocking there would also stop it draining completions. The
  queues hold thousands of plans, so only a real database stall reaches this.

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
identical output for all of them.

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

## What AmmarServer gained for this

The commit pinned in `build.sh` carries all four.

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

## Verification

No new scored run yet. Locally, against a fresh seed with the same 42-check suite:

- `test/test.sh`: **42/42**, with `WORKERS=4 DB_THREADS=2` and again with the defaults ( 32 workers, 4
  database threads ).
- A stress run of 200 concurrent keep-alive connections, 1,000 connections x 10 requests, 30 concurrent
  create+like+read-back sequences, 200 clients that disconnect mid-request, and 20 pipelined request pairs:
  no failed request, no mismatch.
- 300 concurrent sequences of create post, like a missing post, like the new post, read it back. This
  deliberately puts failing likes in the same write transactions as valid creates; every create returned
  201, every missing-post like 404, every valid like 201, and every read-back like_count was 1. The retry
  path was observed taking over 11 rolled-back batches of up to 32 writes.
- 100 sequential create-then-`GET /feed` checks: the new post was in the feed every time, so the shared read
  transaction does not hide a commit that happened before the read request arrived.
- gzip: 5,102 bytes of feed decoded to the same JSON from a 2,160-byte response.
- `/health` answers 200 on both ports, 503 while Postgres is stopped, and recovers after it starts again.

### Local load figures

These are local proxy runs, not the rig: a desktop, the server pinned to its own vCPUs, PostgreSQL 16
pinned to its own vCPUs, `mload` on the rest, 25k–40k users at `-think-scale 0.1 -gzip`. "Before" is this
submission before the one-Sync lane batch, the grouped write commit and ISA-L; "after" includes them, at the
earlier 1 ms window (the 5 ms default's own results are the second table below). No run below had more than
0.012% errors.

| server vCPU | PG vCPU | mload vCPU | users | workers / DB threads | req/s | p95 | p99 | notes |
|---:|---:|---:|---:|---|---:|---:|---:|---|
| 4 | 8 | 4 | 30k | 64 / 4 | 23.7k | 465 ms | 545 ms | before |
| 4 | 8 | 4 | 30k | 32 / 4 | 25.7k | 294 ms | 385 ms | before |
| 4 | 8 | 4 | 30k | 64 / 4 ( repeat ) | 23.9k | 429 ms | 596 ms | before |
| 4 | 8 | 4 | 35k | 32 / 4 | 32.6k | 60 ms | 100 ms | before |
| 4 | 8 | 4 | 35k | 64 / 4 | 31.5k | 151 ms | 221 ms | before |
| 4 | 8 | 4 | 30k | 32 / 4 | **28.2k** | **20.4 ms** | **35.0 ms** | after |
| 4 | 8 | 4 | 30k | 32 / 4 ( repeat ) | 28.2k | 23.0 ms | 37.9 ms | after |
| 4 | 8 | 4 | 40k | 32 / 4 | 32.2k | 417 ms | 447 ms | after, 0.012% 500s |
| 8 | 5 | 3 | 25k | 32 / 4 | 23.4k | 19.3 ms | 56.4 ms | before, PostgreSQL-bound |
| 8 | 5 | 3 | 25k | 32 / 4 | 23.4k | 18.0 ms | 37.9 ms | after, PostgreSQL-bound |

The lane window was swept separately at the same 30k-user load ( 32 / 4, same partition ). Throughput stayed
at ~28.3k req/s throughout; the window buys fewer round trips and wake-ups per request:

| window | server CPU | Postgres CPU | p95 | p99 |
|---:|---:|---:|---:|---:|
| 1 ms | 100% | 100% | 7.7 ms | 19.6 ms |
| 3 ms | ~83% | ~79% | 7.2 ms | 15.9 ms |
| **5 ms ( default )** | **~77%** | **~76%** | **8.8 ms** | **16.6 ms** |
| 10 ms | ~75% | ~68% | 10.8 ms | 16.3 ms |
| 20 ms | ~72% | ~67% | 13.9 ms | 24.7 ms |

The 5 ms default takes most of the saving for about 1 ms more p95 than 1 ms; 10 and 20 ms buy little more
server CPU and start costing latency and tail. Server/Postgres percentages are relative CPU at the same
28.3k req/s.

With that 5 ms default, the same local partition ( server 4 vCPU / PostgreSQL 8 / mload 4 ) held:

| users | req/s | p95 | p99 | errors |
|---:|---:|---:|---:|---:|
| 30k | 28.3k | 8.8 ms | 16.6 ms | 0 |
| 40k | 37.5k | 27.8 ms | 46.2 ms | 0 |
| 50k | 45.7k | 102.5 ms | 214.0 ms | 0 |
| 60k | 48.1k | 408.6 ms | 498.6 ms | 0 |

At 60k users that is a ~1.48M-user request rate; the local mload (4 vCPUs) and PostgreSQL (8 vCPUs) were
the limits and p95 was near 500 ms. The rig has dedicated load generators and a 16-core PostgreSQL box, so
these local limits are not the rig's; they are the floor this server reached on 4 vCPUs.

The sweep's consistent result was **32 workers rather than 64**: at 30k users on 4 server vCPUs, the before
numbers were 25.7k at p95 294 ms for 32 / 4 against 23.7k at 465 ms for 64 / 4, and the repeat of 64 / 4 was
23.9k at 429 ms. `DB_THREADS=4` stayed; `DB_THREADS=2` was mixed, and 6/8 threads or a shorter batching
window hurt once the local PostgreSQL was near saturation. After the three changes, the same 30k-user run
went from 25.7k / 294 ms to **28.2k / 20 ms**. At 40k users the local load generator and PostgreSQL were the
limits; the 500s there are consistent with the bounded plan queue.

The scored run this change is aimed at is the previous revision's 8-core result: 1.02M users at p95 288 ms
with the application at 37% CPU, and the next step ( 1.09M ) failing p95 at 780 ms with the application still
at 38%. The CPU headroom and the latency cliff were the signature of the ring thread waiting on one database
round trip per worker; this revision moves that wait to independent lanes. A loopback measurement cannot
reproduce the database box's real round trip, so the check has to be the benchmark rig.

## Rule notes

- **Rule 3**: one query per request, the spec's SQL as written. Queries are prepared and pipelined, which
  the rule allows. Nothing is moved into Postgres. JSON, timestamps and gzip are all done in C.
- **Rule 5**: `WORKERS * DB_THREADS` connections, capped at 512. The defaults are 32 * 4 = **128**.
- **Rule 7, no caching**: every response comes from a query run while serving it. Prepared statements,
  the connections and the transport's buffers are the only things reused between requests.
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

`WORKERS=N` changes the number of worker processes ( default 32, at least one per port, at most 512 ). `DB_THREADS=N` changes the database threads per worker ( default 4, reduced automatically when
512 / `WORKERS` is smaller ). `DB_BATCH_MIN` and `DB_BATCH_US` tune a lane's coalescing window ( default 8
plans and 5,000 µs ; 0 disables the wait ) so it can be swept from `start.sh` without a rebuild. `build.sh`
honours `AMMARSERVER_SRC=/path/to/a/checkout` to build against a local AmmarServer tree instead of fetching
the pinned commit; that tree has to be at least the pinned commit ( it carries the deferred-response API ).
CI and the benchmark box set neither.
