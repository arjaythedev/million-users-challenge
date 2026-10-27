# Crystal, no framework: io_uring + its own Postgres client, no GC

Crystal with the garbage collector compiled out (`-Dgc_none`): its own io_uring layer through raw
syscalls, HTTP/1.1 parser, router, JSON validator/writer, JWT check, and **its own Postgres
wire-protocol client** driven by the same ring. libcrypto is used for SHA-256 (JWT, SCRAM),
libdeflate for gzip. The Kemal version of the same API is
[`crystal-kemal-faustinoaq`](../crystal-kemal-faustinoaq).

| | |
|---|---|
| Language | Crystal 1.22.1 (official tarball, sha256-checked by `install.sh`), no shards |
| Framework | none: io_uring via raw `io_uring_setup`/`enter`/`register` syscalls, one process per core |
| Driver | its own (`src/pg.cr`): protocol 3.0, SCRAM-SHA-256 / md5 / cleartext auth, extended query protocol |
| Gzip | yes: libdeflate 1.19 (Ubuntu package), level 6, for bodies of 1 KB or more when the client accepts it |
| Needs | Linux 6.1+ |

## Processes and connections

- `bin/server` forks **one worker per CPU** (`WORKERS` to override) and then only watches them; if one
  dies it stops the rest. Each worker has its own `SO_REUSEPORT` listeners on `PORT` and `EXTRA_PORT`,
  its own ring and its own Postgres connections; workers share nothing.
- Accepted sockets are io_uring **fixed (direct) descriptors**: a multishot accept per listener puts
  them straight into the ring's file table, so they never enter the process fd table. Each worker's
  table has room for 524,288 connections (or `RLIMIT_NOFILE`, whichever is lower, with more rings if
  that's the limit).
- Each connection has one **multishot recv** into a provided-buffer ring (4,096 × 2 KB), so an idle
  user holds no buffer at all: its cost is the kernel socket plus a 64-byte record in a `calloc`'d
  table whose untouched pages stay unmapped.
- Postgres: `500 / WORKERS` connections per worker, at most 32 (rule 5's 512 in all), round-robin.

## A request that needs the database

1. The request is parsed in place in the receive buffer, validated (JWT checked on every request, the
   JSON body fully validated without allocating), and its query appended to a Postgres connection's
   output as **Bind / Execute / Sync** on a statement prepared once at start-up. The SQL is the
   spec's, unchanged. Each Sync makes its own implicit transaction, so a failing query fails alone.
2. The client connection is marked busy: anything else it sent stays buffered until the answer is
   out, so responses always leave in request order.
3. At the end of the loop's batch, each Postgres connection's queued queries go out in **one send**:
   every request that arrived in the same wake-up shares the round trips (pipelining).
4. Replies are parsed as they arrive (`DataRow`s straight into JSON, `CommandComplete` for the like,
   `ErrorResponse` with SQLSTATE `23503` → 404) and on `ReadyForQuery` the HTTP response is written
   into the batch's output arena, gzipped if wanted, and sent with one `SEND` from there (no copy).

Session settings at connect: `TimeZone=UTC`, `DateStyle=ISO`, so `created_at` arrives as
`2026-01-01 12:00:00.123456+00` and is rewritten to `2026-01-01T12:00:00.123Z` without parsing a date.

## Where the time goes

- **No allocation on the request path, and no GC.** Buffers are allocated at start-up and grown with
  `realloc`; the request path creates no Crystal objects. RSS stays flat.
- **JWT**: HMAC-SHA256 with the key's inner and outer pad states hashed once, constant-time compare,
  `alg`, `exp`, `sub` checked.
- **Output**: strings escaped 8 bytes at a time (SWAR), `Content-Length` back-filled into a fixed
  slot, the gzip header and body compressed straight into the arena.
- **One `io_uring_enter` per batch** in the common case: it submits the last batch's sends (to clients
  and to Postgres) and waits for the next completions (`DEFER_TASKRUN`, `SINGLE_ISSUER`).

## Local numbers

Ryzen 5 3600 (WSL2), Postgres 18 on the same machine, server on 4 cores (`WORKERS=4`), wrk with
`Accept-Encoding: gzip`, 256 connections:

| | this | Kemal version | `examples/rust` (axum + SQLx) |
|---|---:|---:|---:|
| `/feed` | 7,400 req/s | 4,000 req/s | 3,600 req/s |
| mix (46% feed, 46% post, 7% like, 1% create) | 12,100 req/s | 6,000 req/s | 5,000 req/s |
| same mix, 4,000 connections on `EXTRA_PORT` | 14,700 req/s | | |

Postgres was on the same box and saturated it, so these are not a score.

## Run

```bash
sudo bash install.sh
bash build.sh
DATABASE_URL=postgres://... JWT_SECRET=... HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

## Files

| | |
|---|---|
| `src/main.cr` | reads the env, forks the workers |
| `src/uring.cr` | rings, accept/recv/send/close for clients and Postgres, the batch loop |
| `src/net.cr` | connection records, arenas, request framing, busy/ordering |
| `src/pg.cr` | the Postgres client: connect + auth, prepared statements, pipelined queries, replies → HTTP |
| `src/app.cr` | routes, validation, fixed responses |
| `src/http.cr`, `src/json.cr`, `src/jwt.cr`, `src/buf.cr`, `src/sys.cr` | parsing, output, C bindings |
