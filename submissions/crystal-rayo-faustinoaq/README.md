# Crystal + Rayo + Rayo::PG

The feed API as a normal [Rayo](https://github.com/faustinoaq/rayo) application:
route blocks, `ctx.json`, streaming JSON arrays, `ctx.db.query`, and `Rayo::JWT`.
The hand-written reference is [`crystal-epoll-faustinoaq`](../crystal-epoll-faustinoaq).

| | |
|---|---|
| Language | Crystal 1.22.1, official tarball with SHA-256 verification |
| Framework | Rayo, pinned to `c9553d94583fe2a56ab299e3788fe40dcd0e72b2` |
| Driver | Rayo::PG, prepared statements and asynchronous pipelining |
| JWT | Rayo::JWT, HS256 through OpenSSL libcrypto |
| Gzip | Rayo built-in libdeflate, level 1, responses at least 1 KiB when accepted and smaller compressed |
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

## Framework APIs and remaining helper

`src/server.cr` contains the routes; application helpers live in `Feed` in
`src/support.cr`. Rayo owns both listeners (`app.run ports: [...]`), worker
initialization, gzip (`app.gzip = true`) and command counts (`res.rows_affected`).
There are no Rayo subclasses, private-method hooks or copied compression code.
Updating to c5358ed reduced application source from **292 to 210 lines**.
The existing release build already includes `-Dwithout_mt`, now required by
Rayo's fork guard, as well as `-Dgc_none` and `--mcpu x86-64-v3`.

One small serializer remains: `Feed::Timestamp` truncates UTC Postgres timestamps
to exactly three fractional digits. Rayo's `src/rayo/json/timestamp.cr:96`
emits six digits with no precision option, which fails this challenge's response
contract. A precision option is the remaining upstream candidate. The helper
assumes the UTC/ISO session settings configured by Rayo. No new framework bug
was found in this update. Bcrypt and SQLite are not used by this API.

The epoll feed reference was audited again: all five SQL statements match
exactly, including the correlated like count and feed ordering. Both clients
pipeline Bind/Execute/Sync, send queued work together, and wait for the implicit
transaction's ReadyForQuery before responding. The reference has no aggregated
writes, query-result cache or JWT cache to port; cross-request caching is forbidden
by challenge rule 7. There is no missing reference query optimization to port in
this feed app.

Rayo currently has no automatic Postgres reconnect. The health endpoint returns
503 on query errors or an exhausted/unavailable pool; recovery from a lost database
connection requires restarting the server.

## Validation

With c5358ed, the repository's CI harness and its `test/test.sh` passed
**42/42 checks** locally,
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

## Local sanity comparison

Final comparison: see below. Earlier measurements on old
Rayo revisions are preserved in [the prior session](benchmarks/2026-10-28.txt);
they do not describe the current dependency.

Wave 3 sanity results and PostgreSQL statement costs are recorded in
[the session output](benchmarks/wave3.txt). This short comparison uses mc1 CPUs
0–2, three server workers, PostgreSQL 18 on port 5501 sharing those CPUs, and a
crdev client on CPUs 9–11. Each workload gets a fresh full-seed copy. Both entries
use 32 Postgres connections per worker. The whole session holds the shared
benchmark flock; no run exceeds 3,000 connections. Neither host settings nor
containers are tuned. These are local sanity checks, not million-user capacity
scores or measurements of the scored 8/4-core limits.

| Short sanity workload | Rayo req/s | epoll req/s |
|---|---:|---:|
| `/feed`, wrk, 256 connections, 10 s | 7,374 | 6,219 |
| Mixed mload, 3,000 users, 5 s ramp + 15 s hold | 8,729 | 9,520 |

Both mixed runs had zero request errors and passed the latency thresholds:
Rayo p95/p99 **288/627 ms**, epoll **208/312 ms**. There was no explicit warm-up;
these brief runs include startup/ramp effects. Mixed throughput remains about 8%
below epoll in this session. Rayo uses its built-in gzip level 1; epoll uses level 6.
No compression-level override is exposed by `App` at this revision.

`pg_stat_statements` shows the feed query dominates mixed database execution time
(66% Rayo, 70% epoll). Mean times (Rayo/epoll, ms) were feed **0.499/0.553**,
single post **0.071/0.065**, like **1.033/0.981**, create **1.852/1.370**.
Feed buffer accesses were 149/148 per call in mixed runs and exactly 125/125
in the read-only runs. Total mixed statement execution time per completed SQL
call was **0.351/0.365 ms**. These are execution times including waits, not CPU
measurements; they show no 2.6× database-work gap in this feed session.
The unchanged query shapes and similar buffer work give no basis for adding
application SQL workarounds. Short-run startup, scheduling and serialization
costs remain unisolated; a final clean comparison is still needed.

[Current harness and extra-check output](benchmarks/wave3-check.txt).

## Final comparison (Rayo c9553d9)

`local/wave3-bench.sh` (method above), three rounds, 2026-10-28:

| Round | epoll feed req/s | Rayo feed req/s | epoll mixed req/s, p95 / p99 | Rayo mixed req/s, p95 / p99 |
|---|---:|---:|---:|---:|
| 1 | 653* | 8,127 | 9,797, 196 / 269 ms | 9,940, 192 / 274 ms |
| 2 | 5,990 | 6,489 | 8,929, 328 / 804 ms | 9,961, 196 / 297 ms |
| 3 | 7,509 | 8,059 | 9,953, 181 / 246 ms | 10,068, 184 / 251 ms |

\*First run after restoring the seed template, cold: the feed query averaged
85 ms instead of ~0.3–0.5 ms. All mixed runs had zero errors and passed.
Medians: feed 5,990 vs 8,059 req/s (7,509 vs 8,059 excluding the cold run),
mixed 9,797 vs 9,961 req/s, p95 196 vs 192 ms, p99 269 vs 274 ms.

## Run

```bash
sudo bash install.sh
bash build.sh
DATABASE_URL=postgres://... JWT_SECRET=... HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```
