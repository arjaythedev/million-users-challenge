# Crystal + Rayo + Rayo::PG

The feed API as a normal [Rayo](https://github.com/faustinoaq/rayo) application:
route blocks, `ctx.json`, streaming JSON arrays, `ctx.db.query`, and `Rayo::JWT`.
The hand-written reference is [`crystal-epoll-faustinoaq`](../crystal-epoll-faustinoaq).

| | |
|---|---|
| Language | Crystal 1.22.1, official tarball with SHA-256 verification |
| Framework | Rayo, pinned to `e0006c8b773dae7b75d78eedab314419fb08c0e9` |
| Driver | Rayo::PG, prepared statements and asynchronous pipelining |
| JWT | Rayo::JWT, HS256 through OpenSSL libcrypto |
| Gzip | libdeflate, level 6, responses at least 1 KiB when accepted and smaller compressed |
| Needs | Linux 6.1+, io_uring permitted by the container runtime |

## How it runs

- `App#run` forks one worker per available CPU (`WORKERS` overrides), supervises the
  children, and stops the other workers if one exits. Every worker listens on both
  required ports with `SO_REUSEPORT`. Rayo spreads connections over multiple rings
  when a fixed-file table fills; there is no application idle timeout.
- Each worker opens `min(32, 500 / WORKERS)` Postgres connections, with a minimum of
  one. On the challenge's 4, 8 and 16 cores this stays below the 512-connection limit.
- Every database request executes the spec's SQL unchanged. Statements are prepared
  once per connection; the framework pipelines requests and flushes them together
  each loop turn. Each write waits for `ReadyForQuery` after its implicit commit.
  There are no response, query-result or JWT-verification caches.
- JSON fields preserve the spec's order. Postgres sessions use UTC/ISO timestamps;
  a small value serializer truncates fractions to milliseconds without allocating
  strings or moving formatting into SQL. JWT signatures and expiration are checked
  on every authenticated request, before validating its post id.
- Request bodies and token claims use reusable worker scratch buffers. Rayo keeps a
  deferred request's input alive, so a create callback can decode its body and
  username again without retaining a separate request object.
- Builds use `--release --mcpu x86-64-v3 -Dgc_none -Dwithout_mt`. The normal application
  request path creates no managed objects; buffers are reused or grown with malloc/
  realloc. Rayo retains connection objects when slots are first used, so memory can
  grow as connection count or buffer high-water marks grow. `without_mt` also removes
  Crystal's background runtime monitor allocations, as described in Rayo's CORE.md.

## Submission-local framework additions

`src/server.cr` contains the application routes. Helpers belong to `Feed` in
`src/support.cr` and `src/gzip.cr`; the dependency is neither vendored nor changed.

Candidates to upstream:

- **Multiple listeners:** pinned Rayo's `src/rayo/app.cr:49` creates one listener per
  worker, despite the ring accepting several. `Feed::App` overrides only worker
  initialization to supply both listeners, preserving `App#run` supervision.
- **Affected-row counts:** `src/rayo/pg/conn.cr:215` handles DataRow, ErrorResponse and
  ReadyForQuery but drops CommandComplete (`C`); `Result` exposes no affected count.
  Consequently the two required like responses cannot be distinguished with the
  unchanged SQL. `Feed::CommandComplete` is included into `Rayo::PG::Conn` by a tiny
  compatibility hook. It retains the INSERT count per connection until that query's
  callback; it works across fragmented receives and interleaved connections. This
  uses a private method and must be revisited when updating the dependency.
- **Gzip response middleware:** a reusable libdeflate compressor and output buffer,
  applied through `app.after`, with no compressed-response caching.
- **Timestamp JSON values:** the UTC millisecond serializer could become a generic
  Postgres timestamp helper. Here its assumptions match Rayo's session settings and
  the challenge's modern timestamps.

Rayo currently has no automatic Postgres reconnect. The health endpoint returns
503 on query errors or an exhausted/unavailable pool; recovery from a lost database
connection requires restarting the server.

## Validation

The repository's CI harness and its `test/test.sh` passed **42/42 checks** locally,
including both listening ports. Extra integration checks verified 64 concurrent
likes on one new post (one 201, 63 repeats, final count 1), a 500-character Unicode
body, and byte-identical decoded gzip responses on both ports with `gzip;q=0`
respected. The same TCP connection also answered successfully after 76 seconds
idle. `check.sh` uses the same build/start/test flow,
substituting a uniquely named database and the repository path in a temporary copy
of `test/run.sh`. It removes the harness's child-name kill because Rayo already
terminates its workers when their recorded parent PID exits. It drops its database
on exit and leaves shared `feed_test` untouched.

```bash
PGHOST=127.0.0.1 PGUSER=postgres PGPASSWORD=postgres bash check.sh
```

This requires the challenge's existing `feed_seed`, curl, jq, openssl and psql.

## Local numbers

One comparison session on 2026-10-28: Ryzen 5 3600 / WSL2, three server workers on
`mc1` CPUs 0–2, PostgreSQL 18 at port 5501 sharing those CPUs, client in `crdev`
on CPUs 9–11. Each entry used 32 Postgres connections per worker and a fresh copy
of the full seed. All four runs were inside the shared benchmark `flock`.
Both servers were built in `crdev` with the fixed x86-64-v3 release baseline.

| Workload | Rayo req/s | epoll req/s |
|---|---:|---:|
| `/feed`, wrk, 256 connections, 15 s | 5,907 | 5,322 |
| Mixed mload, 3,000 users over both ports, 35 s | 8,286 | 7,391 |

| Mixed-run result | Rayo | epoll |
|---|---:|---:|
| p95 | 359 ms | 393 ms |
| p99 | 519 ms | 1,505 ms |
| Request errors | 0% | 0% |
| mload latency/error thresholds | pass | fail (p99) |

The mixed run used a 5 s ramp and 30 s hold, `-gzip -think-scale 0.02`:
think times were 125× shorter than scoring's 2.5. No explicit warm-up was used;
reported latency includes ramp-up. These are single runs on a shared local host,
with Postgres sharing the server's CPU budget, **not million-user capacity scores**
or evidence about the scored 8/4-core limits. The three-core worker count also
differs from scoring. [Commands and captured output](benchmarks/2026-10-28.txt)
include the reference revision and exact conditions. Neither run exceeded 3,000
connections.

## Run

```bash
sudo bash install.sh
bash build.sh
DATABASE_URL=postgres://... JWT_SECRET=... HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```
