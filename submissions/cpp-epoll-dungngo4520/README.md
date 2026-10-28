# cpp-epoll-dungngo4520

Million Users Challenge submission in C++17 with no web framework:
a hand-rolled HTTP/1.1 server on Linux epoll, talking to PostgreSQL
through libpq pipeline mode.

## Layout

- `src/server.h` - shared types, config globals, prototypes
- `src/util.cpp` - clock, fatal errors
- `src/json.cpp` - minimal JSON cursor, escape/unescape, body extractor
- `src/jwt.cpp` - in-tree SHA-256, HMAC, base64url, HS256 verification
- `src/http.cpp` - HTTP/1.1 request parser, status text, id parse
- `src/api.cpp` - request lifecycle, routing, response serialization
- `src/conn.cpp` - client socket io, accept, write buffering, LRU
- `src/pg.cpp` - libpq pool: connect, pipeline pump/drain, failover
- `src/worker.cpp` - per-thread event loop, idle sweep, reconnects
- `src/uring.cpp` - optional io_uring backend (see below)
- `src/main.cpp` - environment, fd limits, thread spawn

## Architecture

- Thread-per-core workers (`THREADS`, default `nproc`), each with its own
  epoll set and its own `SO_REUSEPORT` listen socket on both `PORT` and
  `EXTRA_PORT`. No shared state, no locks anywhere.
- Every worker owns `PG_CONNS` libpq connections (default 8, capped so
  total <= 500). Connections are nonblocking, in pipeline mode, with the
  four endpoint queries prepared once at connect time.
- One client request = one query + one pipeline sync, so results pair
  1:1 with the `sent` queue. libpq reads from the socket inside
  `PQsend*`/`PQflush` calls, so the result drain runs after every libpq
  interaction, not only on `EPOLLIN`.
- Requests queue per-connection in arrival order, so keep-alive
  pipelining preserves response order.
- Responses are sent directly with `send(MSG_DONTWAIT)`; a heap write
  buffer is allocated only for partial writes. ~4 KB per idle conn.
- Idle connections are kept `IDLE_MS` (default 90 s, above the required
  75 s) and reaped by an LRU sweep, oldest first.
- HS256 JWTs are verified per request (in-tree SHA-256/HMAC); nothing is
  cached across requests and no results are cached at all.
- JSON is serialized in the app (exact key order, compact bodies);
  `created_at` is truncated to milliseconds in app code.

## Environment

Required: `DATABASE_URL`, `JWT_SECRET`, `HOST`, `PORT`, `EXTRA_PORT`.
Tuning: `THREADS`, `PG_CONNS` (per worker), `IDLE_MS`.
`DATABASE_URL` may be a postgres URI or a libpq keyword string.

## io_uring backend (optional)

`URING=1` swaps each worker's epoll loop for an io_uring ring:
multishot accept on both listeners, multishot recv with a provided
buffer ring per conn, SEND SQEs only for backpressured writes (the
direct `send()` fast path stays), and one-shot poll CQEs wrapped
around the libpq sockets. Any setup failure (kernel too old, memlock
limits) falls back to epoll per worker.

Tuning (per worker): `URING_SQ` (2048), `URING_CQ` (8192),
`URING_BN` (4096 provided buffers), `URING_BS` (4096 bytes each).
Defaults are sized so ~16 workers fit the default 8 MB
RLIMIT_MEMLOCK; raise them when the limit allows.

Measured locally at 40k conns / ~14k req/s: same throughput as
epoll, ~equal CPU/request; at saturation (~31k req/s) about 2-3%
fewer CPU/request. Costs ~64 MB of provided-buffer arena per 4
workers once the buffers are touched. The win here is small because
the epoll path was already at ~6 syscalls/request.

## Build / run

- `install.sh` - installs g++, libpq, libdeflate and liburing dev
  headers on Ubuntu 24.04
- `build.sh` - `g++ -O3 -march=native -flto`, links `-lpq -ldeflate
  -lpthread` plus `-luring` when liburing headers are present
- `start.sh` - execs `./feed_server` in the foreground
