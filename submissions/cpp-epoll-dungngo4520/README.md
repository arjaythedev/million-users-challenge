# cpp-epoll-dungngo4520

Million Users Challenge submission in C++17 with no web framework:
a hand-rolled HTTP/1.1 server on Linux epoll, talking to PostgreSQL
through a native wire-protocol client (SCRAM-SHA-256 auth, prepared
statements, pipelined Sync groups).

## Layout

- `src/server.h` - shared types, config globals, prototypes
- `src/util.cpp` - clock, fatal errors
- `src/json.cpp` - minimal JSON cursor, escape/unescape, body extractor
- `src/jwt.cpp` - in-tree SHA-256, HMAC, base64url, HS256 verification
- `src/http.cpp` - HTTP/1.1 request parser, status text, id parse
- `src/api.cpp` - request lifecycle, routing, response serialization
- `src/conn.cpp` - client socket io, accept, write buffering, conn table
- `src/pg.cpp` - pool scheduling: read/write split, batching, failover
- `src/pgw.cpp` - PG wire protocol: auth, encoders, result parser
- `src/worker.cpp` - per-thread event loop, idle sweep, reconnects
- `src/uring.cpp` - optional io_uring backend (see below)
- `src/main.cpp` - environment, fd limits, thread spawn

## Architecture

- Thread-per-core workers (`THREADS`, default the process cpuset), each
  with its own epoll set and its own `SO_REUSEPORT` listen socket on
  both `PORT` and `EXTRA_PORT`. No shared state, no locks anywhere.
- Every worker owns `PG_CONNS` wire-protocol connections (default 32,
  capped so total <= 500). Submits concentrate on a sticky per-class
  connection, so an event batch merges into one Sync group: reads share
  one implicit transaction per pump pass (up to 128 statements), writes
  commit in bounded groups of up to 16 whose responses hold until
  ReadyForQuery. Sparse event batches pause briefly to let arrivals
  accumulate, which is what makes those groups form. The last quarter of
  the pool prefers writes, so a commit's fsync never head-of-lines reads
  on a shared connection.
- Result field bytes are referenced by offset into the connection's
  receive buffer, so a result can complete in a later parse pass than
  its rows arrived. Completions are budgeted per pass and parked
  conns resume after the event batch - one big Sync group cannot stall
  the whole epoll round.
- Connections live in a per-worker arena (reserved address space, THP-
  advised) plus a dense live list swept incrementally; no per-request
  heap allocation.
- Responses are sent directly with `send(MSG_DONTWAIT)`; a heap write
  buffer is allocated only for partial writes. ~4 KB per idle conn.
- Idle connections are kept `IDLE_MS` (default 90 s, above the required
  75 s) and reaped by the sweep.
- HS256 JWTs are verified per request (in-tree SHA-256/HMAC with the
  secret's midstates precomputed); nothing is cached across requests.
- JSON is serialized in the app (exact key order, compact bodies);
  `created_at` is truncated to milliseconds in app code.
- gzip uses ISA-L (igzip level 0) when libisal is present, libdeflate
  otherwise; bodies >= 1 KB get compressed when the client asks.

## Environment

Required: `DATABASE_URL`, `JWT_SECRET`, `HOST`, `PORT`, `EXTRA_PORT`.
Tuning: `THREADS`, `PG_CONNS` (per worker), `IDLE_MS`.
`DATABASE_URL` is a postgres URI (`postgres://user[:pass]@host/db`).

## io_uring backend (optional)

`URING=1` swaps each worker's epoll loop for an io_uring ring:
multishot accept on both listeners, multishot recv with a provided
buffer ring per conn, SEND SQEs only for backpressured writes (the
direct `send()` fast path stays), and one-shot poll CQEs wrapped
around the PG sockets. Any setup failure (kernel too old, memlock
limits) falls back to epoll per worker.

Tuning (per worker): `URING_SQ` (2048), `URING_CQ` (8192),
`URING_BN` (4096 provided buffers), `URING_BS` (4096 bytes each).
Defaults are sized so ~16 workers fit the default 8 MB
RLIMIT_MEMLOCK; raise them when the limit allows.

## Build / run

- `install.sh` - installs g++, libdeflate, libisal and liburing dev
  headers on Ubuntu 24.04
- `build.sh` - `g++ -O3 -march=native -flto -no-pie -fno-plt`, links
  `-ldeflate -lpthread` plus `-lisal` and `-luring` when present
- `start.sh` - execs `./feed_server` in the foreground
