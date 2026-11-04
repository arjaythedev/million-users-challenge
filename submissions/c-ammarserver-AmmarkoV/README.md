# C + AmmarServer + libpq, deferred responses, served direct on :80 and :81

| | |
|---|---|
| **Language** | C ( C11, gcc 13, `-O3 -march=native -flto`, for AmmarServer as well as the API ) |
| **HTTP server** | [AmmarServer](https://github.com/AmmarkoV/AmmarServer) in API mode on `io_uring`, pinned by commit in `build.sh` ( `80e68cdd` ) |
| **Postgres driver** | libpq 16 ( Ubuntu 24.04's `libpq-dev`, which speaks to Postgres 18 ), prepared statements, pipeline mode, binary results |
| **Compression** | ISA-L 2.31 igzip ( Ubuntu 24.04's `libisal-dev` ), level 0, feed responses only |
| **Crypto** | OpenSSL `libcrypto` for HS256 |
| **Processes** | 1 parent + `WORKERS` worker processes, half on `PORT` and half on `EXTRA_PORT`; `WORKERS` defaults to the affinity CPU count, never below **8** |
| **Database threads** | `DB_THREADS` per worker ( default **2** : one read lane and one write lane ) |
| **Postgres connections** | `WORKERS * DB_THREADS`, capped at the **512** allowed ; **16** on 4- and 8-core runs, **32** on 16 cores |

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
- **One read lane and one write lane.** A worker starts `DB_THREADS` database threads, each with its own
  blocking libpq connection, prepared statements, response buffer and gzip scratch. The default is two: all
  but the last are read lanes, and the last is reserved for `POST /posts` and `POST /posts/:id/like`, so a
  commit never delays a read. One read lane batches harder than several, which is what cut the CPU per
  request; `DB_THREADS` is still a knob.
- **The worker count follows the CPU affinity, with an eight-worker floor.** Eight is not arbitrary: a ring's
  direct-descriptor table holds at most 262,144 connections, and four workers on a 4-core run would cap at
  ~1.05M connections before reaching the 1.4M target. Eight tables give ~2.1M, and a 4-core run is then eight
  workers of ~5,400 req/s each. A 16-core run gets sixteen workers.
- **Responses come back on the ring thread.** A database thread builds the body ( and gzips it ), puts a
  completion on the queue and wakes the worker's eventfd once per completed batch, not once per response. The
  ring thread drains completions and sends each through `AmmServer_APISendDeferred`; a response that finds
  every send buffer busy stays queued and the fd is written again.
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
| 4 / 2 | 8 | 48.8k | 181 ms | 214 ms | 0.019% | 1.60 cores | **32.7 µs** | 0.89 |
| 4 / 3 | 12 | 51.7k | 218 ms | 306 ms | 0 | 2.09 cores | 40.4 µs | 1.25 |
| 4 / 4 | 16 | 46.7k | 499 ms | 567 ms | 0 | 2.13 cores | 45.7 µs | 1.28 |
| 6 / 2 | 12 | 51.5k | 210 ms | 269 ms | 0 | 1.98 cores | 38.5 µs | 1.14 |
| 8 / 2, before batched eventfd | 16 | 51.9k | 71 ms | 96 ms | 0 | 2.18 cores | 42.0 µs | 1.37 |
| **8 / 2 dynamic default (2 runs)** | **16** | **51.8–51.9k** | **57–154 ms** | **68–186 ms** | **0** | **2.13–2.17 cores** | **41.2–41.8 µs** | **1.28–1.36** |

The first row is the old default on the same partition: 83 µs of server CPU per request and only ~30.5k
req/s. The new default holds ~51.8k req/s at ~41 µs/request, with the CPU per request cut by half. Four
workers with two database threads are even cheaper (32.7 µs), but four ring tables cap at ~1.05M
connections, below the 1.4M target, which is why the default has an eight-worker floor. Beyond two database
threads, extra read lanes cost CPU for little throughput: 4/3 and 6/2 are close to 8/2 but use more server
and Postgres CPU, and 4/8 is materially worse.

The target request rate is 43.6k req/s (1.42M on 8 cores, 1.37M on 4). At 41 µs/request that is ~1.8 cores
of the 4-vCPU server; the local partition held 51.8k req/s, and the 4-core p95 was 57–154 ms depending on
host noise. Eight workers × 262,144 ring slots is ~2.1M connections, so a 1.4M run leaves ~175k per worker.
The batched eventfd changed the before/after rows only slightly (42.0 to 41.6 µs, 1.37 to 1.31 context
switches/request); it is included because it is free and reduces wake-up syscalls. A five-minute hold at 50k users on the same 4-vCPU partition held
**52.0k req/s at p95 77.6 ms / p99 106.6 ms, with 0 errors in 14.3M requests and 41.7 µs CPU/request**. Above ~55k local
connections the loopback client itself runs out of ephemeral ports (`cannot assign requested address`), so
60k-user local runs are not meaningful.

The 8- and 16-core runs give the same request rate with more cores, and the local 4-vCPU partition is the
harder case. This remains a projection: the rig's 60 s ramp, 1M sockets and remote Postgres are the only
real test.

## Rule notes

- **Rule 3**: one query per request, the spec's SQL as written. Queries are prepared and pipelined, which
  the rule allows. Nothing is moved into Postgres. JSON, timestamps and gzip are all done in C.
- **Rule 5**: `WORKERS * DB_THREADS` connections, capped at 512. The defaults are 8 * 2 = **16** on 4- and
  8-core runs and 16 * 2 = **32** on 16 cores.
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

`WORKERS=N` overrides the worker count ( default max( 8, affinity CPU count ), at least one per port, at
most 512 ). `DB_THREADS=N` overrides the database threads per worker ( default 2, reduced automatically when
512 / `WORKERS` is smaller ). `DB_BATCH_MIN` and `DB_BATCH_US` tune a lane's coalescing window ( default 8
plans and 5,000 µs ; 0 disables the wait ) so it can be swept from `start.sh` without a rebuild. `build.sh`
honours `AMMARSERVER_SRC=/path/to/a/checkout` to build against a local AmmarServer tree instead of fetching
the pinned commit; that tree has to be at least the pinned commit ( it carries the deferred-response API ).
CI and the benchmark box set neither.
