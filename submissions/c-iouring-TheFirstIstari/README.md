# c-iouring-TheFirstIstari

C11 feed API: one process, N worker threads, io_uring multishot accept with an
epoll fallback, a per-thread libpq connection pool, HS256 JWT auth, and a
picohttpparser-based HTTP layer (vendored, parse-in-place, zero-copy) with a flat hand-written JSON layer. No framework, no router, no ORM.

Passes 6–14 are all in this tree (async libpq, feed-first gzip, binary
protocol, lazy slabs + tick clocks, 64 B hot split, 128-slot wheel, O(1)
close guards, per-slot waiter counts, 50 k saturation). `test/test.sh` is
**42/42 green** on both the plain and PGO builds.

## Stack

| Piece | Exact version (build host) | Ubuntu 24.04 equivalent |
| --- | --- | --- |
| Language | C11, `gcc 16.2.1` | `build-essential` |
| Async I/O | `liburing 2.15-1.1` | `liburing-dev` |
| Postgres driver | libpq from `postgresql-libs 18.6-4.1` | `libpq-dev` |
| Crypto (HMAC-SHA256) | `openssl 3.6.4-1.1` | `libssl-dev` |
| gzip (level 1, feed-first) | vendored `libdeflate 1.26` (static, `vendor/`) | none (vendored) |
| HTTP/1.x parser | vendored `picohttpparser dd573d8` (static, `vendor/`) | none (vendored) |
| Database | `postgres:18` (see `test/run.sh`) | — |

Build flags (`Makefile`): `-O3 -march=x86-64-v3 -flto -Wall -Wextra
-D_GNU_SOURCE`, linked with `-luring -lpq -lcrypto -lz`. The build is
zero-warning.

### Why `-march=x86-64-v3` is safe here

x86-64-v3 means AVX/AVX2/BMI1/BMI2/FMA/LZCNT/MOVBE. The build host is Ryzen
(which has it) and the bench box is Xeon 8260-class (Cascade Lake: Haswell+
ISA, so v3 is covered with room to spare). If a future bench CPU ever predates
v3, drop the flag back to plain `-O3` — nothing else changes.

### PGO is optional and NOT default (Pass 12)

`make pgo` instruments (`-fprofile-generate -DBUILD_PGO_GEN
-Wl,-u,__gcov_dump`), runs the 5 k-user `mload` profile pass
(`pgo-profile.sh`: scratch DB `feed_pgo` on `PGHOST`/`PGPORT`, SIGTERM dump
via the `BUILD_PGO_GEN` hook in `server.c`), drops the instrumented objects
(keeping `.gcda`/`.gcno`), then rebuilds with `-fprofile-use
-fprofile-correction -DBUILD_PGO_GEN`. `-fprofile-correction` tolerates torn
counters from the racing workers; `-DBUILD_PGO_GEN` is kept so `main` keeps an
identical CFG.

Measured delta is **+0.3 %** (noise). CI and `build.sh` keep the plain
`make -j$(nproc)` target — do not default to PGO.

## Process / thread model

`server.c`: one process, `workers = min(ncpu, 16)` pthreads. There is no
master/acceptor process — **every worker binds both ports itself** and owns:

- one epoll fd (+ one io_uring ring while multishot accept is usable),
- one 32-connection Postgres pool (`pg.c`, thread-local),
- thread-local render/scratch buffers (so the hot path never mallocs).

`SO_REUSEPORT` spreads accepted connections across workers in the kernel.
Startup prints `server: <N> workers on <host>:<port>+<extra>` plus per-thread
`pg pool: n/32 conns ok` lines to stderr.

Config is env-only (`server.c`, all required except `HOST`):

| Var | Meaning |
| --- | --- |
| `HOST` | bind address (default `0.0.0.0`) |
| `PORT` / `EXTRA_PORT` | **both ports serve all routes**; `test/run.sh` health-checks both |
| `DATABASE_URL` | libpq conninfo (never logged) |
| `JWT_SECRET` | HMAC secret; server refuses to start without it |

## Postgres pool: why 32 × 16 = 512

`pg.h` / `net.c` (`PG_NSLOTS 32`): each worker keeps 32 blocking libpq
connections (round-robin, single-flight per connection — a connection is only
ever used by its owning thread, so no locking). 16 workers × 32 = **512
server-side connections max**, which fits under the CI/database box's
`max_connections 600` with headroom left for `psql`/seed/test connections.
A slot reconnects on demand (reconnect-once per query, then an inline
non-prepared fallback), and every new connection gets
`SET statement_timeout=5000` plus the 5 prepared statements below. Query
latency is histogrammed to stderr (`pg stats: ops=… slow_gt50ms=…
reconnects=…`, once a minute at most).

Under the 50 k saturation run the pool sat at **busy peak 5/32 per worker,
slow_gt50ms 0** — the DB is not the bottleneck.

## SQL (verbatim, `pg.c`)

```sql
SELECT 1
```
```sql
SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id)::int AS like_count FROM posts p JOIN users u ON u.id=p.user_id ORDER BY p.created_at DESC,p.id DESC LIMIT 20
```
```sql
SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id)::int AS like_count FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=$1
```
```sql
INSERT INTO posts(user_id,body) VALUES($1,$2) RETURNING id,created_at
```
```sql
INSERT INTO likes(user_id,post_id) VALUES($1,$2) ON CONFLICT(user_id,post_id) DO NOTHING
```

Prepared once per connection as `q_health`, `q_feed`, `q_post`, `q_mkpost`,
`q_like`. `PQcmdTuples` distinguishes first like (`201`) from repeat (`200`).
FK/range/invalid-text failures (`23503`, `22003`, `22P02`) on read/like map to
`404 post not found`, never `500`.

## Binary protocol (Pass 8)

`pg.c` sends every query with `PQsendQueryPrepared(..., resultFormat=1)` (params
stay text, results come back binary) and decodes in C: `int2`/`int4`/`int8`
big-endian, `timestamptz` as int64 microseconds since 2000-01-01 UTC. No text
parsing on the feed path.

## Async libpq (Pass 6, no pipeline mode)

Plain async only (`PQsendQueryPrepared` / `PQconsumeInput`, never pipeline
mode) so one slow query no longer head-of-line blocks its worker. While a
request waits (`CONN_PG_WAIT`, `net.h`), the worker keeps the client fd open,
arms the `PQsocket` in the same epoll (`EPOLLIN`, plus `EPOLLOUT` when
`pg_need_write` says libpq still holds unsent bytes), and resumes on readiness.
One in-flight per PG connection is preserved; a `BUSY` slot falls back to the
blocking `pg_run` path. The gzip render happens after the async completion,
before the single `sendmsg`, so the `PG_WAIT` path is untouched.

## HTTP behavior

Routes (`http.c`): `GET /health`, `GET /feed`, `GET /posts/<id>`,
`POST /posts`, `POST /posts/<id>/like`. Anything else is `404
{"error":"not found"}`. Validation order is route → auth → id → body → DB.

- **Dual ports:** `PORT` and `EXTRA_PORT` serve identical traffic (two
  listeners per worker, `g_port`/`g_extra_port` in `server.c`).
- **Keep-alive:** `Connection: keep-alive` by default; idle connections close
  after **80 s** (`IDLE_S`, `net.c`); a connection also closes after ~1000
  requests (`MAX_REQS`). `HTTP/1.0` without `Connection: keep-alive` closes.
- **Header timeout:** a connection that has completed zero requests and still
  holds unparsed bytes is closed **20 s** after accept (`HDR_TIMEOUT_S`), so
  slow-loris header drips cannot pin a slot. Idle reaping runs on a 1 s tick
  over a **128-slot timing wheel** (`WHEEL_N`, `net.c`, Pass 11; per-tick cost
  ~104 ns reported) — only slots in `(last_sweep, now]` expire, O(expired).
- **Caps:** 16 KiB headers + 16 KiB body (`HDR_CAP`/`BODY_CAP`); oversized
  bodies fail the request. At most 64000 connections per worker
  (`MAX_CONNS_PER_WORKER`); excess accepts are dropped immediately.
- **EAGAIN stash (Pass 9):** a partial `sendmsg` stashes the unsent tail in
  `conn_cold.wbuf/wlen/woff` and re-arms `EPOLLOUT` — no response is ever
  dropped on a full socket.

## Recv (Pass 18: per-thread pbuf ring, 1024x2KB, epoll fallback)

Each worker recvs into the next slot of a fixed 2 MB thread-local pbuf ring and parses in place (zero-copy); the slot is returned by cursor advance at handler return and only partial/pipelined requests are boxed into the size-class slab. The ring is also `PROVIDE_BUFFERS`-registered with the io_uring instance (bgid 1); recv-multishot support is probed at startup and logged, while the data path stays on the epoll fallback. Per-idle-conn owned memory is `conn_hot` (64 B) + `conn_cold` (128 B) in one malloc block (~220 B with overhead, no read buffer on the fast path) -- under 500 B by construction.

## Gzip (Pass 7: feed-first, level 1, ≥1024 B; Pass 15: libdeflate)

`GET /feed` is compressed whenever the client sends `Accept-Encoding: gzip`
(measured **5303 → 1832 B, ~2.9×** with libdeflate
level 1 on the reference feed); `GET /posts/<id>` is compressed when asked
**and** the body is ≥ 1024 bytes (`GZIP_MIN_POST`). Everything else (health,
errors, `POST /posts`, likes, small posts) stays `identity`. Responses carry
`Content-Encoding: gzip` plus the compressed `Content-Length`; clients without
`Accept-Encoding` get byte-exact identity JSON (golden files unchanged). Any
compression failure falls through to identity.

Compression reuses one per-thread libdeflate level-1 compressor
(`libdeflate_alloc_compressor(1)`, gzip framing via
`libdeflate_gzip_compress()`) and a 136 KiB thread-local output buffer
(`GZ_CAP 139264`) — no per-request malloc, and the render buffers (`t_feed`
128 KiB, `t_post` 8 KiB) are gzipped after rendering, before the single
`sendmsg`. libdeflate 1.26 is vendored (`vendor/libdeflate-1.26.tar.gz`,
SHA256-pinned, static `libdeflate.a` built by `build-vendor.sh`); no system
zlib is needed (`-lz` removed from the link).

## Auth: JWT verified on every request, nothing cached

`jwt.c` `jwt_verify()` runs per protected request (`POST /posts`,
`POST /posts/<id>/like`): split `h.p.s` → reject `=` padding/extra segments →
base64url-decode the header → require it to contain `"alg"` followed by
exactly `"HS256"` (quote-aware: `find_key` in `json.c` skips `"alg"`
occurrences inside string literals, so `{"note":"alg HS256"}` cannot spoof
it) → base64url-decode the 32-byte signature → HMAC-SHA256 over `h.p` with
constant-time compare → `sub` must be a JSON **string** of digits, no leading
zero (else `401 invalid token payload`) → `username` must be a JSON string
(used verbatim as `author`) → `exp` must be a JSON **number** greater than
`now` (missing/stale `exp` is `401 invalid or expired token`, never a payload
error). Only the secret pointer/length is memoized per thread to skip
`strlen`; tokens and claims are never cached. Embedded NUL bytes terminate C
strings at the first NUL, so they cannot smuggle content past validation.

## IDs, bodies, timestamps

- Post ids are strict `^[1-9][0-9]*$`, ≤ 15 digits, ≤ int8max (`valid_id`,
  `http.c`); anything else is `400 invalid post id`. Out-of-range numerics that
  reach Postgres surface as `22003`/`22P02` and still map to `404`.
- `POST /posts` needs a present `Content-Length`, a JSON object with a string
  `body` (missing/non-string → `400 body is required`), trimmed non-empty,
  ≤ 500 Unicode code points (`json_charlen` counts UTF-8 lead bytes) or
  `400 body must be at most 500 characters`. Malformed JSON →
  `400 malformed JSON body`. Auth is checked before the body is parsed.
- `created_at` renders `YYYY-MM-DDTHH:MI:SS.mmmZ` (`fmt_ts_micros`, `pg.c`):
  the PG `timestamptz` binary microseconds are **truncated to milliseconds,
  never rounded** (civil-date conversion from the int64 micros).

## Hot/cold split, slabs, clocks (Passes 9–10)

`net.h`: `struct conn_hot` is compile-time asserted to **≤ 64 B and 64 B
aligned** (`_Static_assert(sizeof <= 64)` + `alignof == 64`); one
`posix_memalign(64)` block holds hot at offset 0 plus `struct conn_cold` at
+64. The per-request hot path touches only the hot line; `created_at`, the
EAGAIN stash, and the `pg_pending` stash live in cold. Backing is 64 B
aligned throughout (`slab_take` uses `posix_memalign(64, …)`).

- **Lazy 2 K slab (Pass 9):** no read buffer until the first bytes arrive (1 M
  conns × 0 B instead of 1 M × 4 K). First `recv` allocates `SLAB_INIT 2048`,
  doubling to at most 32 KiB (16 KiB header + 16 KiB body); a drained oversized
  slab shrinks back to 2 K.
- **Slab pools (Pass 10):** size classes 2 K/4 K/8 K/16 K/32 K, LIFO, capped
  64/32/16/8/4 per worker (`SLAB_CAPN`). Steady state does zero malloc;
  `MALLOC_ARENA_MAX=2` in `start.sh` bounds glibc arena sprawl (share-nothing
  workers + pooled slabs already avoid churn). Allocator A/B test showed
  **identical** results.
- **Tick clocks (Pass 9):** one `clock_gettime` + one `time()` per event-loop
  tick per worker replaces per-request `time(NULL)`×2; `net_now_wall()` serves
  the cached wall clock.

## O(1) close + waiter guards (Passes 12–13)

- **Closed-set (Pass 12):** every `conn_free` records into a per-tick
  `t_closed[256]` array (`CLOSED_CAP`); client events in the same `epoll_wait`
  batch check membership O(1) w.r.t. connection count instead of scanning the
  128-slot wheel. Overflow sets a flag and falls back to the exact wheel scan
  for the rest of that tick, so the verdict stays airtight.
- **Waiter counts (Pass 13):** every `PG_WAIT` conn is counted under its
  claimed slot (`t_pg_waiters[32]`, inc on defer / dec on completion-sweep /
  close). `net_gc_pg` disarms waiter-less slots and the poll path skips
  waiter-less PG events without scanning the wheel — O(32) slots, not O(conns).

## Per-connection memory budget

`net.h` + `net.c`: ~64 B hot + ~100 B cold + one pooled receive slab (2 K
lazy, up to 32 K worst case). Steady-state feed traffic sits near **~2.3 KB
per conn**. No per-request malloc on the hot path — rendering uses
thread-local buffers (`t_feed` 128 KiB, `t_post` 8 KiB, JWT payload 8 KiB).

Measured RSS: **68 MB @15 k conns, 110 MB @30 k, 149 MB @50 k** (~2 KB/user).
Projection: 1 M conns ≈ 2.3 GB + kernel socket state, well under a 32 GB box.
FDs track **users + 579** (listeners, epoll, rings, 512 PG conns, stdio).

## Non-default settings

| Setting | Value | Where | Why |
| --- | --- | --- | --- |
| `statement_timeout` | 5000 ms (`SET` + connect `options=`) | `pg.c` | bound worst-case query latency |
| `connect_timeout` | 5 s | `pg.c` | fail fast on dead DB |
| `application_name` | `c-iouring` | `pg.c` | identify conns in `pg_stat_activity` |
| `MALLOC_ARENA_MAX` | 2 | `start.sh` | bound glibc arena sprawl |
| `ulimit -n` | 1048576 (or best effort) | `start.sh` | 64 k conns × workers + PG fds |
| `TCP_NODELAY` | 1, listeners + accepted | `net.c` | small JSON, no Nagle delay |
| `TCP_DEFER_ACCEPT` | 1 s (Linux only) | `net.c` | fewer wakeups from half-open conns |
| `SO_REUSEPORT`/`REUSEADDR` | on | `net.c` | kernel fan-out to all workers |
| `listen()` backlog | 65535 | `net.c` | absorb burst accepts |
| io_uring ring | 2048 SQ entries, `SINGLE_ISSUER \| COOP_TASKRUN` | `net.c` | multishot accept, low syscall cost |
| Multishot accepts | 2 per listener; throwaway-loopback probe at startup; runtime `-EINVAL` ×4 falls back to `accept4` | `net.c` | portable: fast path where supported, epoll everywhere else |
| Slab classes | 2 K/4 K/8 K/16 K/32 K, LIFO capped 64/32/16/8/4, 64 B aligned | `net.c` | zero-malloc steady state |
| Idle / header timeouts | 80 s / 20 s, 1 s wheel sweep, 128 slots | `net.c` | slow-loris resistance, O(expired) reap |
| Requests per conn | ~1000 then close | `net.c`/`http.c` | bound per-conn state lifetime |
| PG reconnect | reconnect-once + inline non-prepared retry | `pg.c` | survive restarts without dropping |
| Gzip | libdeflate L1, feed-first, ≥ 1024 B post threshold | `http.c` | ~2.9× feed saving, no small-body tax |

## Validation results (last measured, Passes 6–14)

| Check | Result |
| --- | --- |
| `test/test.sh` | **42/42 green**, plain build and PGO build |
| 15 k saturation | 543 / 0.9 / 1.3, err 0 %, blip-free (reported `RESULT` triplet) |
| 30 k saturation | 1146 / 0.9 / 1.3, err 0 %, blip-free |
| 50 k saturation | 1698 / 0.9 / 1.5, err 0 %, blip-free; one ~10 s PG checkpoint dip self-recovered |
| RSS | ~68 MB @15 k, ~110 MB @30 k, ~149 MB @50 k |
| FDs | users + 579 |
| PG pool | 512 max (32×16) vs `max_connections 600`; busy peak 5/32, `slow_gt50ms` 0 |
| Allocator A/B | identical (pooled slabs; arenas capped) |
| PGO | +0.3 % — kept optional, **not default** |

`bench/mload` 5 k reference point from the base pass: 0 failures,
p95 ~1.1 ms, p99 ~1.6 ms.

## Files

`server.c` (env, `min(ncpu,16)` threads, PGO dump hook) ·
`net.c`/`net.h` (listeners, io_uring probe, epoll loop, 128-slot wheel,
closed-set, waiter counts, 64 B hot/cold, slab pools, tick clocks) ·
`http.c`/`http.h` (parser, routes, validation, feed-first gzip) ·
`pg.c`/`pg.h` (pool, 5 prepared SQL, binary decoders, async + blocking paths,
renderers) · `jwt.c`/`jwt.h` (HS256 verify, every request) ·
`json.c`/`json.h` (escape/decode/scan, error bodies) ·
`Makefile` (`-O3 -march=x86-64-v3 -flto`, `pgo`, `clean`) ·
`install.sh` (root-aware, pinned) · `build.sh` (`make -j`) ·
`start.sh` (foreground `exec`, env-only) · `pgo-profile.sh` (5 k profile workload) ·
`.gitignore` (`server`, `*.o`, `*.gcda`, `*.gcno`).

`make clean` removes only `server`, `*.o`, `*.gcda`, `*.gcno` — sources stay.

## Scripts

- `install.sh`: root-aware (`sudo` only when non-root and present, never reads
  passwords), pinned on CachyOS/Arch (`liburing=2.15-1.1`,
  `postgresql-libs=18.6-4.1`, `openssl=3.6.4-1.1`) with latest-fallback,
  Ubuntu 24.04 equivalents (`build-essential liburing-dev libpq-dev libssl-dev`),
  plus `curl`/`tar` to fetch the pinned libdeflate tarball (no system zlib:
  gzip is vendored, see `build-vendor.sh`).
- `build-vendor.sh`: verifies the `libdeflate 1.26` tarball SHA256, extracts
  to `vendor/`, and builds the static `libdeflate.a` (no system install);
  invoked automatically by the Makefile, `fetch`-only mode from `install.sh`.
- `build.sh`: `make -j$(nproc)` from the submission dir.
- `start.sh`: foreground `exec ./server`, env-only, `ulimit -n 1048576 ||
  true` + `MALLOC_ARENA_MAX=2`.
- `pgo-profile.sh`: scratch-DB 5 k `mload` profile driver (not CI).
- All `*.sh` are executable; `.gitignore` covers `server *.o *.gcda *.gcno *.a` plus `vendor/*` (except the pinned `*.tar.gz`).

## CI and PR status

CI (`.github/workflows`): clean Ubuntu 24.04, `postgres:18` service,
`ALTER SYSTEM SET max_connections = 600` (as on the database box), then
`seed/make-seed.sh`, `install.sh`, `test/run.sh <dir>`.

PR #16 is **stale (Pass 4 vintage)** — it predates Passes 6–14 (async libpq,
gzip, binary protocol, slabs, hot/cold split, wheel, O(1) guards, 50 k
saturation) and the current README/scripts. Do not merge it as-is; open a fresh
PR from this tree when ready. No commit/push/PR was made by this docs pass.
