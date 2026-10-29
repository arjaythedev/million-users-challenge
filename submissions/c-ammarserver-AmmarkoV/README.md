# C + AmmarServer + libpq, prefork, served direct on :80 and :81

| | |
|---|---|
| **Language** | C ( C11, gcc 13, `-O3 -march=native -flto`, for AmmarServer as well as the API ) |
| **HTTP server** | [AmmarServer](https://github.com/AmmarkoV/AmmarServer) in API mode on `io_uring`, pinned by commit in `build.sh` |
| **Postgres driver** | libpq 16 ( Ubuntu 24.04's `libpq-dev`, which speaks to Postgres 18 ), prepared statements, pipeline mode, binary results |
| **Compression** | libdeflate 1.19 ( Ubuntu 24.04's `libdeflate-dev` ), gzip level 1, feed responses only |
| **Crypto** | OpenSSL `libcrypto` for HS256 |
| **Processes** | 1 parent + `WORKERS` ( default **32** ) worker processes, half on `PORT` and half on `EXTRA_PORT` |
| **Postgres connections** | one per worker : **32** ( of the 512 allowed ) |

This is my [$12 server challenge](https://github.com/arjaythedev/twelve-dollar-server-challenge) submission
moved onto 16 cores and Postgres. The HTTP side, the routing, the JSON writing and the HS256 verification are
the same code; SQLite became libpq, and one process became 32, each answering its requests in batches.

## Design

AmmarServer's API mode is one thread owning every connection from `accept` to `close` and answering each
request from a synchronous C callback, with each connection a *direct descriptor* in an `io_uring` file table
( so an idle keep-alive connection costs a ~32 byte struct and no file descriptor ). That was ideal with
SQLite in-process, where a request was ~60 µs of CPU and nothing else. With Postgres on another machine a
request is a network round trip, and a synchronous callback waiting on one serves nobody else meanwhile.

So the parallelism comes from **processes**, not from making the callback asynchronous:

- `main()` forks `WORKERS` processes and then only waits on them. Each worker is the old single-threaded
  server: one ring, one thread, one **blocking** libpq connection, its own prepared statements, its own
  buffers and its own HMAC key schedule. Nothing is shared between workers, so there is still no lock
  anywhere between a request and its answer.
- Every listening socket has `SO_REUSEPORT`, so the kernel spreads new connections evenly over the 16
  workers of each port, and each worker owns its connections for their whole life. At a million users that
  is ~31,000 connections and ~1,300 req/s per worker.
- One Postgres connection per worker, used only by that worker's thread, so there is no pool and no pool
  wait.
- **Requests are answered in batches.** AmmarServer collects every request that became ready in one pass
  over the worker's ring and hands them to one handler call ( `AmmServer_SetAPIBatchHandler` ). The handler
  sends all of their queries to Postgres in one libpq pipeline and reads all of the results back in one
  round trip. Nothing waits for a batch to fill: when it is quiet a batch is one request, and what arrives
  while a batch waits on Postgres becomes the next one. So a busy worker shares its round trips, and its
  sleeps and wake-ups, between several requests, instead of queueing them one behind another.

This stays synchronous: a batch blocks its worker exactly as one request used to. Fully asynchronous
responses would need a deferred-response API in AmmarServer's transport, which is a much larger change.
Batching gets the shared round trips without it.

**Why 32 workers.** With batching, fewer and busier workers mean bigger batches: fewer round trips, fewer
sleeps and wake-ups, and less work for Postgres too. But a worker's pipeline runs one statement after
another inside its single Postgres backend, so too few workers leaves Postgres without enough parallelism.
Measured ( see below ), 32 was the best of 16, 32, 64 and 128: 16 could not keep up at ~55,000 req/s, and 64
cost more CPU per request and failed the latency limit at that rate.

## Optimizations, and why

**Prepared once, binary in and out.** The five statements are the spec's SQL verbatim, `PQprepare`d when
a worker connects. Parameters go over in binary ( an `int8` is 8 bytes; the post body is its own bytes
with a length, so it never needs NUL terminating ), and results come back in binary: ids and counts are
big-endian integers, and `created_at` is microseconds since 2000-01-01 UTC. That is formatted in C, so
there is no text timestamp to parse and nothing depends on the session's `TimeZone` or `DateStyle`.

**Timestamps written by hand.** A feed formats 20 of them. Instead of `gmtime_r` and `snprintf`, the date
comes from Howard Hinnant's `civil_from_days` and the digits are written straight into the buffer. Checked
against the `gmtime_r` version over 2.18 million values from 1900 to 2400, including every day boundary:
identical output for all of them.

**Pipelined queries, one transaction each.** Each statement in a batch is followed by its own `Sync`, so it
is its own implicit transaction, exactly as outside a pipeline. A like on a missing post fails alone and
cannot roll back another user's new post in the same batch. Tested with 450 mixed parallel writes, including
150 likes on a missing post: every new post committed and read back with its own body.

**Compiler flags.** AmmarServer is built with the same `-O3 -march=native -mtune=native -flto=auto` as the
API, and the final link optimises across both, so AmmarServer's small per-field response writers can be
inlined into the handlers. Everything is built from source on the machine that runs it, which is what makes
`-march=native` safe. The hardening flags AmmarServer sets ( `-fstack-protector`, `_FORTIFY_SOURCE=2` ) are
kept: this server faces the internet.

**gzip with libdeflate, level 1, feed only.** `mload -gzip` sends `Accept-Encoding: gzip` on every request,
and the spec says the network is part of the test. On the seed's feed:

| | bytes | per response |
|---|---|---|
| uncompressed | 5,102 | |
| libdeflate level 1 | **1,901** | **15 µs** |
| libdeflate level 6 | 1,866 | 29 µs |
| libdeflate level 9 | 1,863 | 33 µs |

Level 1 is 1.9% bigger than level 6 at half the CPU. A single post ( ~300 bytes ) barely shrinks, so only
bodies of 1 KB or more get compressed. libdeflate compresses a whole buffer in one call with a compressor
allocated once per worker, so no per-request compression state is allocated ( the gzip middleware buffer
allocations were what held back the Go example ).

**One `send()` per response, JSON written straight into the response buffer.** This is unchanged from the
$12 server. AmmarServer writes the headers backwards into a reserve in front of the body, and the handlers
write JSON through bounds-checked writers with no intermediate document.

**The HS256 key schedule is prepared once per worker.** The two SHA-256 states after `key^ipad` and
`key^opad` depend only on the secret, so they are copied per token instead of recomputed: 1.48 µs a token
down to 0.25 µs. Every signature is still computed over the bytes that arrived, and `exp` is still checked
( rules 7 and 9 ).

## What AmmarServer gained for this

Three things this submission needed went into AmmarServer itself, as API that any handler can use. The
commit pinned in `build.sh` has all three.

- **`AmmServer_StartAPIOnRingsReusingPort`**: `AmmServer_StartAPIOnRings` with `SO_REUSEPORT` set on the
  listening socket before it is bound. This is what lets 64 workers each bind their own socket on the
  same port. It is a separate function rather than the default, because with it on, a second server
  started on a port by mistake would quietly take a share of the connections instead of failing to bind.
- **`AmmServer_APIResponse.extraHeaders`**: header lines a handler adds to its response, here
  `Content-Encoding: gzip\r\n`. `AmmServer_APIResponseReset` clears it, so an error answer that replaces
  a half-built gzipped one never carries the gzip header.
- **`AmmServer_SetAPIBatchHandler`**: the batches described above, on the `io_uring` transport. A request
  set aside for its batch waits in its connection's own partial-request buffer, the same place a request
  waiting for a send buffer already waited, so the kernel's receive buffer goes straight back. The single
  handler stays for the epoll transport and for anything answered before the batch handler is set.

## Measurements

Measured on my own machine ( Ryzen 7 3800X, 8 cores / 16 threads ) with Postgres 16 and `mload` on the same
machine over loopback, so read these as costs and comparisons, not as scores. The server was pinned with
`taskset` to **4 vCPUs** ( 2 physical cores, both hyperthreads of each ), with Postgres and `mload` on the
other 12. There is no network round trip here, at most 130,000 connections were open, and these cores are
faster than a Xeon 8260 vCPU. Users were spread over 8 loopback addresses, because past ~28,000 per address
the load generator runs out of source ports and stalls ( which looks like a server stall but is not ).

**The 1M-user request rate on 4 vCPUs:** 100,000 users at `-think-scale 0.25 -gzip`, ~42,000 req/s, which is
what a million users ask for in the benchmark. "Before" is this submission before batching, the hand
written timestamps and the new compiler flags.

| version | workers | server CPU per request | server CPUs busy | Postgres CPU | p95 | p99 | errors |
|---|---|---|---|---|---|---|---|
| before | 128 | 79 µs | 88% | 6.35 cores | 194 ms | 393 ms | 0% |
| now | 128 | 74 µs | 84% | 6.22 cores | 104 ms | 165 ms | 0% |
| now | 64 | 64 µs | 73% | 5.11 cores | 35 ms | 89 ms | 0% |
| now | **32** | **54 µs** | **61%** | **4.22 cores** | **33 ms** | **79 ms** | 0% |
| now | 16 | 43 µs | 48% | 3.44 cores | 95 ms | 138 ms | 0% |

**Past it:** 130,000 users, ~54,000 req/s, still on 4 vCPUs.

| workers | req/s | server CPUs busy | p95 | p99 | errors | |
|---|---|---|---|---|---|---|
| **32** | 54,359 | 80% | **116 ms** | **183 ms** | 0% | **pass** |
| 64 | 51,577 | 93% | 579 ms | 906 ms | 0% | fail |
| 16 | 37,162 | 55% | 2.6 s | 6.5 s | 0% | fail: Postgres-side parallelism, not CPU |

At the default 32 workers, 4 vCPUs carried 1.3 times the 1M-user request rate, with the server's CPUs 80%
busy. The parts this machine cannot show are the kernel's cost of a million open sockets and a real network
round trip to the database box. On 16 vCPUs there is room for both.

**Batching, directly.** With all traffic sent to one worker at ~2,100 req/s, it averaged 1.21 voluntary
context switches per request. Without batching each request costs at least 2: one sleep waiting on the ring,
one waiting on Postgres.

## Rule notes

- **Rule 3**: one query per request, the spec's SQL as written. Queries are prepared and pipelined, which
  the rule allows. Nothing is moved into Postgres. JSON, timestamps and gzip are all done in C.
- **Rule 5**: 32 connections by default. `WORKERS` is capped at 512, one connection each.
- **Rule 7, no caching**: every response comes from a query run while serving it. Prepared statements,
  the connection and the transport's buffers are the only things reused between requests.
- **Rule 8, committed before responding**: every write is its own transaction, ended by its own `Sync`,
  and its response is built only after its result has been read, which comes after the commit.
- **Rule 10, crashes**: a worker that dies is reported and not restarted. The others keep serving their own
  connections. Every worker has `PR_SET_PDEATHSIG`, so none outlives the parent.
- **Rule 13, nothing tuned on the box**: the only setting changed is this process's own `RLIMIT_NOFILE` soft
  limit, raised to its hard limit. Under the benchmark's `LimitNOFILE=2097152` the two are already equal.

## Files

| | |
|---|---|
| `feedapi.c` | the prefork parent, the five endpoints and routing, the batched libpq pipeline, gzip |
| `jwt.c/.h` | HS256 verification: base64url, the distinct 401s the spec asks for, `CRYPTO_memcmp` |
| `install.sh` | apt: `build-essential cmake git libssl-dev libpq-dev libdeflate-dev ca-certificates` |
| `build.sh` | fetches AmmarServer at the pinned commit, builds its library target only, links `build/feedapi` |
| `start.sh` | `exec build/feedapi` in the foreground |

`WORKERS=N` changes the number of worker processes ( 32 by default, at least one per port, at most 512 ).
`build.sh` honours `AMMARSERVER_SRC=/path/to/a/checkout` to build against a local AmmarServer tree instead
of the pinned commit. CI and the benchmark box set neither.
