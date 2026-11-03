# Nish / standard library

Nish 0.26.0 (a statically typed TypeScript subset compiled to native code through LLVM), built with clang-18 and
lld-18 from LLVM 18, against PostgreSQL 18. The compiler runs on Node.js 22.18.0 at build time only. The server is one
native binary with no runtime dependency beyond libc. Every import is Nish's own standard library:
`nish/net/http1-server` for HTTP/1.1, `nish/net/http-compress` for gzip, `nish/net/postgres` for the PostgreSQL wire protocol, `nish/json` and
`nish/json-tokenizer`, `nish/crypto/hmac` and `nish/net/prefork`. No third-party package is used.

```sh
sudo bash install.sh
bash build.sh
DATABASE_URL=postgres://feed:pw@db:5432/feed JWT_SECRET=million-users-challenge HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

## What carried over from the multicore challenge

This is a port of this author's multicore-challenge entry (nish-std-amritk) to the feed API. That challenge put
Postgres and the server on one 8-core box, and its biggest win was batching reads into "read plans". This one gives
Postgres its own 16-core box and asks for SPEC.md's queries, one per request (rule 3), so read plans don't apply.
The cost that matters is the server's CPU per request and its memory per idle connection, times a million.

Carried over:

- **Workers that share nothing.** A prefork parent starts one worker per CPU and restarts none of them (rule 10).
  Each worker binds both `PORT` and `EXTRA_PORT` with `SO_REUSEPORT` and `TCP_NODELAY`, and runs one epoll loop over
  its two listeners, its keep-alive connections and its PostgreSQL connections.
- **Idle connections hold no buffers.** With `Http1Config.idleInput`, a connection between requests gives its input
  and head buffers back to pools its worker shares. Locally, 15,000 open connections took 30 MB of RSS in all, about
  2 KB each. Nish 0.23.1's HTTP/1 server no longer walks every connection on every loop turn: at a million
  connections, any per-turn work that grows with the number of connections would be fatal.
- **Prepared, pipelined Postgres.** Each worker has its own read pool and write pool, so a commit's WAL flush never
  holds up reads queued behind it on a connection. Up to 16 groups are pipelined on each connection. Every statement
  is prepared once per connection. `plan_cache_mode=force_generic_plan`, `jit=off`,
  `max_parallel_workers_per_gather=0`, `DateStyle=ISO` and `TimeZone=UTC` go in the startup packet as session
  options. `enable_memoize` is already off server-wide in this challenge's `postgresql.conf`.
- **Rows straight into JSON.** Columns are read in place from the receive buffer: ids and like counts are copied as
  their digits, text goes through the JSON writer's escaper, and timestamps go through `PgText.timestamp`, which
  truncates to milliseconds. No string is made per row, and nothing is allocated per request once the worker has
  served its peak.
- **A wake-up batching window** (from the top entries here, below). After a loop turn that handled fewer than 32
  events, the worker sends what it has and waits 2 ms on an empty epoll set before the next turn, so one wake-up
  serves more events and their queries go to Postgres together. At 1.1M users on 8 cores it took server CPU from
  207% to 153% and read p95 from 2.1 s to 0.5 s.
- **Packed reads, binary results** (Nish 0.26.0's `PgConfig.pack` and `bindStartBinary`). A worker's reads fill
  one connection up to 16 in flight before the next, so their results come back in fewer, fuller packets; writes
  stay spread over their connections, so commits flush side by side. Reads ask for binary results: the timestamp
  is 8 bytes instead of about 29 characters of text and needs no parsing. At 1.1M users on 8 cores the database
  box's retransmits fell from about 2,600 to 1,500 a second, and a 5-minute hold went from p95 2.4 s to 104 ms.
- **Tokens.** HS256 over an HMAC keyed once with `JWT_SECRET`, adapted from the multicore entry's `jwt.ts`: each
  signature copies the two keyed SHA-256 states. The signature, `alg` and `exp` are checked on every request, and
  nothing is cached between requests.

## Rules

- **Queries:** SPEC.md's SQL, one statement per request, each in its own Sync, so its own implicit transaction.
  Writes are committed before the response goes out. The `INSERT ... ON CONFLICT DO NOTHING` row count gives
  `already_liked`, and SQLSTATE 23503 gives 404 `post not found`.
- Nothing in the database is created or changed. Only session settings are used.
- **No caching:** every request reads Postgres while it is being served. Only connections and prepared statements
  are reused.
- **Connections:** by default 48 read and 64 write in total, split evenly over the workers (3 + 4 per worker on 16
  cores). That is 112 of the 512 allowed. See "Measured on Synteq boxes" for why reads get fewer connections than
  writes.
- **Compression:** when `Accept-Encoding` asks for gzip, bodies of 1 KB or more are gzipped at the fast level (Nish
  0.24.0's `HttpCompressor` and `Http1Connection.respondEncoded`). In practice that is only the feed: 5,102 bytes go
  out as 1,899. Smaller bodies (a post, a like, an error) are sent as they are, and `Vary` is left off because no cache
  sits in front of the server.

## Configuration (env, all optional)

| Variable | Default | |
|---|---|---|
| `HTTP_WORKERS` | the CPUs | worker processes |
| `DB_READ_CONNS` | 48 | total read connections, split over the workers |
| `DB_WRITE_CONNS` | 64 | total write connections, split over the workers |
| `DB_MAX_IN_FLIGHT` | 16 | groups pipelined on one connection |
| `HTTP_SLOTS` | 1048576 | connections per port per worker (the search goes up to 3,000,000 users on 4 cores: 375,000 per port per worker) |
| `GZIP_LEVEL` | 1 | deflate level (1 = fast, 6 = default) |
| `GZIP_MIN` | 1024 | smallest body that is gzipped; larger than any body (e.g. 999999) turns gzip off |
| `IDLE_TIMEOUT_MS` | 120000 | idle keep-alive connections are closed after this (SPEC.md asks for at least 75 s) |
| `BATCH_MS` | 2 | after a loop turn with fewer than `BATCH_MIN` events, pause this long so the next turn serves more at once (0 = off) |
| `BATCH_MIN` | 32 | see `BATCH_MS` |
| `DB_PACK` | on | reads fill one connection to `DB_MAX_IN_FLIGHT` before the next (`0` spreads them over all) |
| `DB_BINARY` | on | reads ask Postgres for binary results (`0` for text) |

## Measured on Synteq boxes

Laid out like the official runs: the server on a 16-vCPU / 32 GB app box with 5 IPs (set up with
`box/setup-app-box.sh`), Postgres 18 on its own 24-vCPU box (`box/setup-db-box.sh`, `box/postgresql.conf`), and
`mload` on three load boxes (24, 16 and 16 vCPU) against every app IP and both ports, with a shared `-start`, a
different `-offset` per process, merged at the end. Every run started from a fresh `feed` (`box/reset-db.sh`), with
`-gzip -think-scale 2.5` and a 300 s ramp at 1M (about 3,333 new connections a second).

| users | read + write conns | hold | req/s in hold | p95 | p99 | errors | app box CPU | result |
|--:|:-:|--:|--:|--:|--:|--:|--:|:--|
| 500,000 | 128 + 32 | 2 min | 21,000 | 10.2 ms | 23.0 ms | 0.001% | ~15% | pass |
| 1,000,000 | 128 + 32 | 5 min | 42,000 | 409 ms | 1,335 ms | 0.000% | ~26% | fail (p99) |
| 1,000,000 | 448 + 64 | 5 min | 40,000 | 1,460 ms | 2,131 ms | 0.000% | ~20% | fail |
| 1,000,000 | 64 + 16 | 5 min | 42,000 | 447 ms | 2,706 ms | 0.000% | ~23% | fail (writes: p99 7 s) |
| 1,000,000 | **48 + 64** | 5 min | 42,000 | **272 ms** | **659 ms** | 0.000% | ~21% | **pass** |
| 1,000,000 | **48 + 64** (repeat) | 5 min | 42,000 | **221 ms** | **429 ms** | 0.000% | ~21% | **pass** |
| 1,000,000 | **48 + 64**, 2 ms window, Nish 0.25.0 | 5 min | 42,000 | **227 ms** | **519 ms** | 0.001% | ~17% | **pass** |
| 1,000,000 | **48 + 64**, window, packed binary reads, Nish 0.26.0 | 5 min | 42,000 | **78 ms** | **232 ms** | 0.001% | ~18% | **pass** |

At 1M the server used about 2 of the 16 cores and 1.5 GB of RSS. The latency tail comes from packet loss between
the VMs rather than from CPU: the database box retransmits 1,700–7,500 segments a second over its handful of
connections, and on a pipelined connection one lost segment stalls every query queued behind it until it is
resent. The connection counts set how much each loss costs:

- **Reads:** fewer connections means fuller packets. Going from 128 to 448 read connections spread the same results
  over more, smaller packets, more of them were lost (the database box went from ~3,000 to ~7,500 retransmits a
  second) and p95 went from 409 ms to 1.46 s. 48 read connections took read p99 to about 430–670 ms.
- **Writes:** every write waits for its commit's WAL flush, and the flushes on one connection run one after
  another. With 16 write connections (one per worker) writes queued up to a p99 of 7 s. With 64 they flush in
  parallel and write p99 is about 230 ms.

### 8 and 4 cores

The same boxes with the app box cut to 8 or 4 online CPUs (`/sys/devices/system/cpu/cpuN/online`, then
`box/setup-app-box.sh` again so the RPS mask matches), the same defaults (so 6 + 8 or 12 + 16 connections per
worker), and 2-minute holds as in the search's first step:

| cores | users | p95 | p99 | errors | app box CPU | result |
|--:|--:|--:|--:|--:|--:|:--|
| 8 | 1,000,000 | 202 ms | 251 ms | 0.000% | ~40% | pass |
| 8 | 1,100,000 | 2,069 ms | 3,049 ms | 0.001% | ~42% | fail |
| 8 | 1,250,000 | 7,105 ms | 9,021 ms | 0.163% | ~40% | fail |
| 4 | 1,000,000 | 96 ms | 241 ms | 0.001% | ~68% | pass |
| 4 | 1,250,000 | 5,595 ms | 6,760 ms | 0.325% | ~68% | fail |

With the 2 ms batching window and Nish 0.25.0, on 8 cores:

| users | hold | p95 | p99 | errors | app box CPU | result |
|--:|--:|--:|--:|--:|--:|:--|
| 1,100,000 | 2 min | 236 ms | 504 ms | 0.001% | ~38% | pass |
| 1,100,000 | 5 min | 2,426 ms | 3,205 ms | 0.001% | ~37% | fail |
| 1,250,000 | 2 min | 5,377 ms | 6,627 ms | 0.001% | ~36% | fail |
| 1,250,000 (`BATCH_MS=4`) | 2 min | 5,999 ms | 7,105 ms | 0.001% | ~35% | fail |

With packed reads and binary results too (Nish 0.26.0), on 8 cores:

| users | hold | p95 | p99 | errors | app box CPU | result |
|--:|--:|--:|--:|--:|--:|:--|
| 1,100,000 | 5 min | **104 ms** | **239 ms** | 0.001% | ~35% | **pass** |
| 1,250,000 | 2 min | 5,116 ms | 6,059 ms | 0.001% | ~37% | fail |
| 1,400,000 | 2 min | 8,006 ms | 8,843 ms | 0.001% | ~37% | fail |

Every failure looks the same: feed and post reads take seconds while writes stay at a p95 of about 220 ms, the app
box still has CPU to spare, and both boxes retransmit 2,000–3,000 segments a second. Past a million users the
database sends the app box about 1 Gbit/s of rows, and on these VMs' network the read connections stall on lost
packets before the server runs out of CPU.

## Credits

This is a port of this author's multicore-challenge entry, and the parts that carried over (above) came partly from
other entries there, each written from the idea, not the code. Multicore challenge entries are
[arjaythedev/multicore-server-challenge](https://github.com/arjaythedev/multicore-server-challenge) PRs:

- **c-iouring-tigokraft ([#10](https://github.com/arjaythedev/multicore-server-challenge/pull/10)):** one worker per
  core with its own `SO_REUSEPORT` listeners and its own Postgres connections, nothing shared; idle keep-alive
  connections that own no buffer; pipelined prepared statements over a hand-written wire-protocol client.
- **csharp-sockets-jh-d ([#22](https://github.com/arjaythedev/multicore-server-challenge/pull/22)):** writes on their
  own connections, so a commit's WAL flush doesn't stall pipelined reads; JSON written straight from the rows.
- **rust-tokio-Scooter1337 ([#7](https://github.com/arjaythedev/multicore-server-challenge/pull/7)):** no buffers held
  while a socket is idle; prepared generic plans.
- **rust-mio-foovoo ([#11](https://github.com/arjaythedev/multicore-server-challenge/pull/11)):** each worker's
  Postgres sockets in the same epoll loop as its client sockets.
- **go-raw-kimmotamio ([#37](https://github.com/arjaythedev/multicore-server-challenge/pull/37)):**
  `plan_cache_mode = force_generic_plan` as a session option.

From this challenge's entries ([arjaythedev/million-users-challenge](https://github.com/arjaythedev/million-users-challenge) PRs):

- **c-epoll-unorig-username ([#10](https://github.com/arjaythedev/million-users-challenge/pull/10)),
  java-epoll-stohirov ([#2](https://github.com/arjaythedev/million-users-challenge/pull/2)) and
  rust-tokio-unorig-username ([#11](https://github.com/arjaythedev/million-users-challenge/pull/11)):** the wake-up
  batching window, a short pause after a light loop turn so the next one serves more events and sends their
  queries together.
- **c-epoll-unorig-username (#10) and rust-tokio-unorig-username (#11):** packing reads onto as few connections as
  will take them, and binary result columns (with java-epoll-stohirov (#2) and c-iouring-einhornimmond
  ([#12](https://github.com/arjaythedev/million-users-challenge/pull/12))).

## Local measurements

These were taken in a 4-vCPU dev container, with Postgres 16, `mload` and the server all on one machine, so they are
not comparable to the benchmark boxes. The server ran on one core (`taskset -c 0`, `HTTP_WORKERS=1`) and `mload` on
two others, with 15,000 users at `-think-scale 0.2`, `-gzip`, a 15 s ramp and a 40 s hold:

| | req/s | p95 | p99 | errors | server CPU ticks |
|---|--:|--:|--:|--:|--:|
| this server, gzip (default) | 6,998 / 6,984 | 1.6 / 1.7 ms | 6.3 / 7.6 ms | 0 | 1,794 / 1,755 |
| this server, gzip off (`GZIP_MIN=999999999`) | 6,995 / 6,994 | 1.3 / 1.2 ms | 2.9 / 2.5 ms | 0 | 1,507 / 1,388 |
| the same API in Rust (own epoll HTTP and Postgres client), libdeflate gzip | 6,991 | 4.7 ms | 37.5 ms | 0 | 2,205 |
| the same Rust server, gzip off | 6,988 | 3.1 ms | 7.9 ms | 0 | 1,963 |

The two Nish rows alternated, run by run. RSS was 30 MB with the 15,000 connections open, about 2 KB per connection.
On loopback gzip costs about 23% more server CPU, against 12% for libdeflate in Rust. It cuts the feed's bytes by
2.7x, and the box's network is part of the test (SPEC.md), so it stays on.

`test/run.sh`: 42/42.
