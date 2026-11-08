# typescript-bun-tbsvttr: just rewrite it in Bun

The feed API (`SPEC.md`) in one TypeScript file on Bun, with **zero dependencies**: no `package.json`, no
`node_modules`. HTTP is `Bun.serve`, Postgres is `Bun.SQL`, gzip is `Bun.gzipSync`, HS256 is `node:crypto`.
All of these are built into the runtime. The Node example (Fastify + node-postgres) held 587,000 users. This
entry is the same JavaScript with a different runtime.

## Stack

| Piece | Version |
|---|---|
| Runtime | Bun 1.4.4, the official `bun-linux-x64.zip` release (`aarch64` for local runs), SHA-256 pinned in `install.sh`, in `/opt/bun` |
| HTTP | `Bun.serve` (uWebSockets/uSockets underneath), HTTP/1.1 keep-alive, `routes` for the five endpoints |
| Postgres driver | `Bun.SQL`, Bun's built-in Postgres client (pure Zig, no libpq) |
| JWT | hand-written HS256 check on `node:crypto` `createHmac` + `timingSafeEqual` (~30 lines) |
| JSON | `JSON.parse` for the request body; the response JSON is built by string concatenation |
| Compression | `Bun.gzipSync` with `library: "libdeflate"`, level 1 |

`build.sh` runs `bun build server.ts --target=bun` into `dist/server.js`. There is nothing to install, so
nothing needs pinning besides Bun.

## Process model

- **One worker process per CPU.** `start.sh` sets `WORKERS` from `nproc`, which reads the affinity mask, so
  systemd's `AllowedCPUs`/`CPUAffinity` or `taskset` decide the count. The primary only spawns the workers and
  restarts any that die. On one CPU there is no primary: the single process does the work.
- **Every worker listens on both ports with `reusePort: true`** (`SO_REUSEPORT`). The kernel spreads new
  connections over the workers, and from then on each worker owns its connections. Workers share nothing: each
  has its own event loop, its own `Bun.SQL` pool and its own listeners.
- A worker's stdin is a pipe from the primary. When the pipe closes, the worker exits, so workers never outlive
  the primary, even after a `kill -9`.

## Connections

- `idleTimeout: 120` s. Users idle up to ~40 s between requests; the spec asks for at least 75 s, and Bun's
  maximum is 255 s.
- An idle keep-alive connection is a uSockets socket. No JS object exists for it until a request arrives.
- `maxRequestBodySize: 64 KB` (bodies over 16 KB are out of scope).

## Database

- **One `Bun.SQL` pool per worker**: `min(64, max(4, 256 / WORKERS))` connections each, which is 16 per worker
  and 256 in total on 16 cores, under the 512 limit. `DB_POOL` overrides the per-worker size. `idleTimeout` and
  `maxLifetime` are off, so the pool opens connections on demand and keeps them. Under load Bun.SQL opens
  another connection rather than queueing behind a busy one. 200 concurrent requests on 10 workers opened all
  250, so plan on the full pool being in use.
- The queries are SPEC.md's SQL, verbatim, as tagged templates: one statement per request, in autocommit. Bun
  prepares each one once per connection as a named statement and reuses it.
- Like on a missing post: Postgres raises SQLSTATE `23503`, which `Bun.SQL` puts in `PostgresError.errno`, and
  that becomes 404 `post not found`. `already_liked` comes from the row count of `ON CONFLICT DO NOTHING`.
- `created_at` comes back as a `Date`. `toISOString()` gives UTC with milliseconds and a `Z`.

## Responses

- Post objects are written as strings in the spec's key order. `id` (BIGINT) arrives as a digit string and is
  written out unquoted.
- gzip at level 1 (libdeflate) when the client sends `Accept-Encoding: gzip` and the body is 1 KB or more. In
  practice that is the feed: 5.1 KB becomes 1.9 KB (zlib at level 1 gives 2.4 KB and saves ~3 µs per feed). A
  single post is ~300 bytes and is sent as is.

## Auth

HS256 only. The header must decode to JSON with `"alg":"HS256"`, and the signature is compared in constant time
against an HMAC-SHA256 computed over `header.payload` on every request. `exp` is required and must be in the
future; `nbf` is checked when present. `sub` must be a positive integer string and `username` a string. Nothing
is cached.

## Run it

```bash
sudo bash install.sh
bash build.sh
DATABASE_URL=postgres://user:pass@host:5432/feed JWT_SECRET=million-users-challenge HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

## Results

`test/run.sh`: **passed=42 failed=0**, after `install.sh` on a clean `ubuntu:24.04` (x86_64) against `postgres:18`
with `max_connections = 600`, the same steps CI runs.

Also checked by hand, since the suite doesn't cover these:

- `created_at` is truncated, not rounded: `.123999` comes out `.123Z`, `.999999` comes out `.999Z`. The seed has
  no sub-millisecond timestamps, so the golden checks can't catch rounding.
- gzip: the feed with `Accept-Encoding: gzip` is 1,844 bytes and decompresses to the plain response; a single
  post is sent uncompressed.
- `EXTRA_PORT` serves the whole API, not just `/health`. Both ports have one `SO_REUSEPORT` listener per worker.
- Keep-alive: 200 requests over one connection; a connection idle for 80 s still answers.
- `Expect: 100-continue` on `POST /posts` works.
- Database unreachable: `/health` returns 503 with the error, `/feed` returns 500 `internal server error`, and both
  recover when the database is back.
- A killed worker is restarted. Killing the primary with `kill -9` takes every worker with it.

### Local load tests (not the benchmark hardware)

Docker on an Apple Silicon Mac (native arm64, 10 vCPUs, 16 GB). The server's container is pinned to 1 or 2 cores
with `--cpuset-cpus`, so `nproc` and the worker count follow. Postgres 18 (`box/postgresql.conf` with the memory
settings scaled down) and `mload` each have their own cores. The server has 6 IPs, so `mload` doesn't run out of
source ports. Unless noted, the profile is the scoring one: `-gzip -think-scale 2.5`.

| Server | Cores | Load | Result | Server CPU (hold) | Server RSS |
|---|---:|---|---|---:|---:|
| Bun | 2 | 125,000 users, 40 s ramp, 3 min hold | **pass**: p95 3.9 ms, p99 20.2 ms, 0 errors | ~0.45 cores | 167 MB |
| Node example | 2 | the same | pass: p95 3.2 ms, p99 120 ms, 0 errors | 0.84 cores | 1,011 MB |
| Bun | 2 | 250,000 users, 80 s ramp, 2 min hold | fail on p95 (1.09 s), 0 errors | 1.2–1.6 cores | 213 MB |
| Bun | 1 | 10,000 users, `-think-scale 0.5` (~2,100 req/s) | p95 0.9 ms | 0.21 cores (~99 µs/req) | 48 MB |
| Node example | 1 | the same | p95 2.0 ms | 0.43 cores (~200 µs/req) | 171 MB |
| Bun | 1 | 10,000 users, `-think-scale 0.05` | ~19,500 req/s, p95 221 ms, 0 errors | 1.00 core (saturated) | 72 MB |

- **Not conclusive at 250,000:** the p95 spikes there came with garbage collections in `mload` and with the
  whole Docker VM saturated (server, Postgres and `mload` near their core limits at once). All 250,000
  connections stayed up with 0 errors.
- **Memory:** an open keep-alive connection costs the server under 1 KB, so a million is about 1 GB of RSS.
- **Per request:** about half the CPU of the Node example.
- **Scaling to the box:** this setup is kinder than the benchmark box. The Node example passes 125,000 on 2 cores
  here but held 587,000 on 16 cores there, so these numbers can't be scaled linearly to the box.
- **A rough estimate:** the Node example's README gives ~65% of one box core at ~1,940 req/s, against 0.43 cores
  here, so a box core is about 1.7× slower than these. Then a million users (~42,000 req/s) should need under
  half of the 16-core box's CPU. Not measured: the kernel's share at a million connections.
