# Rules check: fsharp-uring-delneg

Upstream README rules (read 7 Nov 2026 at `d229b63`), SPEC.md and the maintainer's review notes on other PRs
(LEADERBOARD.md footnotes, scoring comments). Every kept change is checked against this list; the log is at the end.

| # | Rule | How this entry complies |
|---|---|---|
| 1 | any language, framework, driver, processes, threads | F# on .NET 11 RC 3, no framework; one process |
| 2 | PostgreSQL 18 at `$DATABASE_URL` only; no Redis/Memcached/SQLite; no server-wide settings | only Postgres; no `SET`, no `ALTER SYSTEM/DATABASE/ROLE` |
| 3 | the SPEC's queries, one per request; prepare, pipeline, batch writes OK; no work moved into Postgres | the five SPEC statements verbatim (`LIMIT 20` literal), prepared once per connection, one per request; reads share a `Sync`, writes group-committed |
| 4 | schema and data fixed | no DDL |
| 5 | at most 512 Postgres connections | 64 read + 32 write in total (split over the threads); startup fails if threads x per-thread > 512 |
| 6 | 42/42 on `test/run.sh` | checked on 4 and 8 cores, buffer ring and PROVIDE_BUFFERS fallback |
| 7 | no caching across requests (responses, query results, tables, JWT verifications) | every request reads Postgres while it is served; nothing about a token is kept; kept between requests: connections, prepared statements, receive buffers, the HMAC key pads, per-thread scratch buffers |
| 8 | writes committed before the response | a write's response waits for the `ReadyForQuery` of its `Sync` (after the implicit transaction commits); `synchronous_commit` untouched |
| 9 | HS256 signature and `exp` checked on every request; no benchmark detection | signature, `alg`, required `exp`, `sub`/`username` types checked per request; no load-test detection |
| 10 | runs alone on the box; OOM or crash loses | one process, bounded memory, exceptions contained per connection |
| 11 | serve ports 80 and 81 directly | `PORT` and `EXTRA_PORT`, `SO_REUSEPORT` listeners per thread |
| 12 | built from source by install.sh/build.sh; pinned deps; `/health` within 60 s | SDK, FSharp.Core (lock file), ISA-L and libdeflate tarballs pinned and SHA-checked; no prebuilt binaries |
| 13 | no tuning beyond `setup-app-box.sh`: no kernel parameters, no CPU pinning | only in-process settings (socket options on our own sockets, GC, threads); no sysctl, no affinity; install/build/start never install or change a kernel |

Reviewer notes on other entries that apply here:

- write replies only after the `Sync`'s `ReadyForQuery`, not at `CommandComplete` (#60, #65, #27, #83)
- a JWT without `exp` must be rejected (#38, #44, #75)
- nothing read outside the submission's folder: no box-wide `/proc/meminfo`, `/proc/stat` (#41, #47, #65, #75, #81)
- no CPU pinning (#61); our `PIN_THREADS` knob is off by default and stays off
- files stay in the submission's own folder (#17, #36)
- vendored sources must match upstream (#16): we download pinned, checksummed tarballs instead

## Per-change log

| change | rules touched | verdict |
|---|---|---|
| no Kestrel host (plain `Microsoft.NET.Sdk` console app) | 1, 12 | ok |
| `TCP_QUICKACK` cleared and `TCP_THIN_LINEAR_TIMEOUTS` set on our listening sockets (inherited by accepted sockets) | 13: socket options on our own sockets are in-process settings, not kernel parameters (the sysctl `tcp_thin_linear_timeouts` stays 0). Go #65 does both; its review found no rule issue | ok |
| re-arm every receive that found no buffer (buffer-ring mode) | none | ok |
| pure-F# gzip (`GZIP_IMPL=fsharp`, Deflate.fs): own RFC 1951/1952 encoder | 12 (no new dependency) | ok |
| Postgres dispatch knobs: `PG_SPREAD` (rotating fill start), `PG_RUN=r` (runs of r to the least-loaded connection) | 3 (still one statement per request, same SQL), 5 (same connection count) | ok |
| `PG_THIN` / `PG_QUICKACK_OFF`: socket options on our own Postgres client sockets | 13 (in-process, our sockets; no sysctl) | ok |
