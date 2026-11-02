# forth-gforth: a from-scratch epoll server in Forth (libpq to Postgres)

The feed API from `SPEC.md`, implemented in **gforth** with no web framework and no
HTTP library: a hand-written single-threaded **epoll** event loop, one process per core,
talking to PostgreSQL through **libpq** via gforth's C FFI. HTTP/1.1, JSON, and JWT HS256
are all hand-rolled Forth. The design goal is the one thing the challenge rewards: hold a
very large number of idle keep-alive connections in a small amount of RAM.

This is the sibling of our $12-server-challenge entry (same author, same core engine); the
data layer was swapped from SQLite to libpq and the server was made multi-process for the
16-core box.

## Stack

| Piece | Version / detail |
|---|---|
| Language / runtime | gforth 0.7.3 (`gforth 0.7.3+dfsg-9build4.1`, the Ubuntu 24.04 archive build) |
| HTTP | hand-written HTTP/1.1 parser + response builder (`http.fs`), no library |
| JSON | hand-written compact emitter + strict request parser (`json.fs`), byte-exact |
| JWT | hand-written HS256 verify (`jwt.fs`) over **libcrypto** `HMAC`/`EVP_sha256` via FFI |
| Postgres driver | **libpq** (`libpq-dev` from the Ubuntu 24.04 archive, PostgreSQL 16 client), bound through gforth's `c-library` FFI in `ffi.fs` / `pg.fs` |
| Sockets / epoll | raw `socket`/`bind`/`listen`/`accept4`/`epoll_*` via a small C shim in `ffi.fs` |
| Postgres server | 18 (on its own box) |

Everything is built from source on the box: gforth is interpreted, and its `c-library`
compiles the one FFI wrapper `.so` (libpq + libcrypto + the socket/epoll shim) at load time.
`install.sh` installs the toolchain from the Ubuntu archive (the distro release is the pin);
`build.sh` clears gforth's FFI cache and forces the wrapper to compile so a broken link fails
the build, not the first request.

## Process and thread model

- **One process per core.** gforth is single-threaded, so parallelism is `WORKERS` worker
  **processes** (default: `nproc`; set on the box to the core count). `start.sh` exports
  `WORKERS=$(nproc)` if unset.
- With `WORKERS>1`, the main process is a thin **supervisor**: it `fork()`s the workers and
  blocks in `wait()`. If any worker exits, the supervisor SIGTERMs the whole process group and
  exits, so a crash brings the service down loud rather than running silently degraded
  (rule 10; systemd's `KillMode=control-group` is the same intent).
- Each worker is independent: its **own** libpq connection, its **own** epoll instance, and its
  **own** listen sockets on both `PORT` and `EXTRA_PORT` opened with **`SO_REUSEPORT`**, so the
  kernel load-balances accepted connections across workers. No shared state, no locks.
- **One request in flight per connection** (the load is non-pipelined): the worker reads a
  request, runs the one SQL statement, writes the response, and moves on. The epoll loop is
  level-triggered with a fixed per-worker connection **slot pool** (O(1) free-list, fd→slot
  map), so accepting and closing connections is allocation-free in steady state.

## Database access

- **libpq, synchronous prepared statements.** At startup each worker connects, runs
  `SET TIME ZONE 'UTC'` on its session (rule 2 allows session-level `SET`), and prepares the
  five statements from `SPEC.md` **verbatim** (`$1`/`$2` placeholders, `(SELECT count(*) …)::int`
  for `like_count`, `INSERT … RETURNING id, created_at`, `INSERT … ON CONFLICT DO NOTHING`).
  One statement per request (rule 3); nothing is moved into Postgres.
- **Connection count.** One connection per worker: 16 workers → **17** Postgres connections
  total (including the health check). Far under the 512 cap — measured constant at 17 regardless
  of client count. Each connection serves many clients because the per-query cost is dominated
  by the network round trip, and a million users is only ~42k req/s.
- **Writes are durable before the response** (rule 8): autocommit with Postgres's default
  `synchronous_commit = on`, so the `INSERT` is committed by the time the 201 is buffered. No
  group-commit/WAL machinery (that was the SQLite entry).
- **Like on a missing post:** the `likes.post_id` foreign key fails with SQLSTATE `23503`, which
  the data layer maps to 404 `post not found` — no separate existence query.
- **`created_at`:** libpq returns `TIMESTAMPTZ` as text (e.g. `2025-12-31 23:59:19.409123+00`);
  the server reformats it in Forth to `YYYY-MM-DDTHH:MM:SS.mmmZ`, **truncating** (not rounding)
  to milliseconds and zero-padding when the fraction is shorter. The `UTC` session guarantees a
  single `+00` shape to parse. Verified byte-exact against the golden files.

## Optimizations, and why

| What | Why |
|---|---|
| **~8 KB RSS per connection** | The whole thesis. A fixed slot pool with small per-connection buffers (4 KB read + 8 KB write, lazily faulted) and no per-connection thread/task. Measured 7.84 KB/conn on the benchmark box (see below). The video's framework entries were 28–29 KB/conn, which is why none of them fit a million in 32 GB. |
| **One process per core + `SO_REUSEPORT`** | Uses all cores from a single-threaded runtime without locks; the kernel balances connections. Each worker is a clean failure domain. |
| **Per-worker prepared statements, text params** | Parse/plan once per connection; `PQexecPrepared` with text params avoids per-request SQL building and keeps the 512-connection budget irrelevant. |
| **Connection shedding OFF by default** | The $12 entry shed idle keep-alives under pool pressure (fd limit 65k, one process). Here the box grants `LimitNOFILE=2097152` and the *goal* is to hold ~1M idle connections, so shedding is off: every connection stays keep-alive. Forcing reconnects at a million clients would cause dial storms through the single-RX-queue NIC. (`SHED=1` re-enables it for experiments.) |
| **No TCP keep-alive probes** | We never set `SO_KEEPALIVE`; at hundreds of thousands of idle connections, periodic probes contend on the single network queue (a known cost in the video's Go follow-up). Dead peers are cleaned up via epoll `HUP`/`ERR`. |
| **Load the JWT secret once** | `jwt-verify` used to `getenv`+copy the secret on every authenticated request; `perf` showed `getenv` in the hot path. It's fixed for the process lifetime, so it's now cached on first use. |
| **Bulk JSON emit** | The JSON string emitter copies maximal "safe" runs with one `move` (a 256-entry safe-byte table) instead of per-byte work; plain-ASCII bodies (the load) emit in a single copy. |

Left deliberately un-done (measured, not worth the risk here):

- **gzip** — optional, and the video showed framework gzip middleware is a per-request ~1 MB
  memory trap that OOMs under load. Our edge is memory; we don't spend it on compression buffers.
  Responses are small (feed ~5 KB). A streaming zlib pass that allocates only while writing is a
  possible later lever if the network becomes the limit.
- **Async/pipelined libpq in the epoll loop** — the benchmark showed the DB is not the
  bottleneck (CPU near-idle, 17 connections), so synchronous libpq stays for simplicity.
- **Caching an `EVP_MAC_CTX`** — OpenSSL 3's one-shot `HMAC()` re-fetches the digest provider
  each call (~2% on the write path only); caching the context would remove it but expands the FFI
  for little gain while we are memory-bound.

## Measured

**Correctness:** `test/run.sh` passes **42/42** on Postgres 18, on both the primary port and (for
`/health`) the extra port, in a single run and under the default multi-worker configuration.

**Scale (benchmark fleet: app `c7i.4xlarge` 16 vCPU/32 GB, Postgres 18 on its own
`c7i.4xlarge`, load generators on separate boxes, same subnet; app box tuned per
`box/setup-app-box.sh`; `mload -gzip -think-scale 2.5`, the scoring parameters):**

| Connections (established) | app server RSS | p95 | p99 | errors | Postgres conns | 16-core load avg |
|--------------------------:|---------------:|----:|----:|-------:|---------------:|-----------------:|
| 20,001                    | ~0.2 GB        | 2.3 ms | 2.9 ms | 0.000% | 17 | ~0.1 |
| 120,001                   | 1.16 GB        | 2.8 ms | 4.1 ms | 0.000% | 17 | ~0.3 |
| 200,001                   | 1.79 GB        | 3.3 ms | 5.3 ms | 0.000% | 17 | ~0.8 |

The marginal cost is **7.84 KB RSS per connection** ((1.79−1.16) GB / 80k), which matches a
separate 8.0 KB/conn measurement. Extrapolated, **one million connections ≈ 7.9 GB** of server
RSS on the 32 GB box, leaving ~24 GB for the kernel's TCP socket memory. CPU was near-idle
through 200k (a million users is ~42k req/s because of the ~24 s think time), and the Postgres
connection count stayed at 17. The binding constraint for "a million on 16 cores" is memory per
connection, and this design has roughly 3–4× headroom over the best framework entry in the video.

(The benchmark fleet's ceiling was the single app IP — a literal 1,000,000-socket run needs
several server IPs and more load generators, as the scoring setup uses. The per-connection memory
and the flat latency/CPU curve are what the numbers above establish.)

## Files

`ffi.fs` (libpq + libcrypto + socket/epoll FFI), `pg.fs` (connection, prepared statements,
result decode, `created_at` reformat), `jwt.fs` (HS256), `json.fs` (emit + parse),
`http.fs` (parse/route/handlers/response), `epoll.fs` (event loop, slot pool, multi-process
bootstrap), `server.fs` (load order + start), `compile-check.fs` (build-time load check),
`install.sh`, `build.sh`, `start.sh`.

## Running locally

```bash
bash seed/make-seed.sh                                   # builds feed_seed
sudo bash submissions/forth-gforth-FantasticForth/install.sh
bash test/run.sh submissions/forth-gforth-FantasticForth # build + start + 42 checks
```

Config is entirely from the environment (`SPEC.md` runtime contract): `DATABASE_URL`,
`JWT_SECRET`, `HOST`, `PORT`, `EXTRA_PORT`, plus optional `WORKERS` (default `nproc`).
