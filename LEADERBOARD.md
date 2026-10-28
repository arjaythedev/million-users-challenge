# Leaderboard

Every submission, scored on the same boxes with the same load test. New PRs are benchmarked as they come in and
added here, so this is the only leaderboard.

**The machines.** The app box is a Synteq VPS with 16 dedicated vCPUs (Xeon Platinum 8260) and 32 GB RAM on
Ubuntu 24.04, set up with [`box/setup-app-box.sh`](box/setup-app-box.sh) and nothing else. Postgres 18 runs on its own
16-core box with [`box/postgresql.conf`](box/postgresql.conf). Load comes from separate 16-core load generators in
the same datacenter, running [`bench/mload`](bench) with `-gzip -think-scale 2.5`.

**How each entry is scored.** Built from source with its own `install.sh` (root) and `build.sh` (user `challenge`),
checked with the 42-check test suite, then started by systemd as `challenge` on a fresh copy of the seed for every
run. Two-minute runs climb (each passing run jumps to where its CPU says ~85% would be), then narrow down to within 5%; a 5-minute hold confirms the
number, stepping down 5% until one passes (p95 under 500 ms, p99 under 1 s, under 1% errors). **Each entry gets the
most users held for 5 minutes on 16 cores, on 8 and on 4**. 16 cores is capped at 1,000,000 (past that the one
Postgres box is the limit); 8 and 4 cores are searched up to 3,000,000. `(max)` = held the search's ceiling. Ranked
by 16 cores.

| # | Submission | PR | Language | Stack | 16 cores | 8 cores | 4 cores | Scored |
|---:|---|---|---|---|---:|---:|---:|---|
| 1 | [`rust-tokio-cknutson12`](submissions/rust-tokio-cknutson12) | [#1](https://github.com/arjaythedev/million-users-challenge/pull/1) | Rust | thread-per-core tokio, hand-written HTTP/1.1, pipelined tokio-postgres | 1,000,000 (max) | 1,210,000 | 1,130,000 | 2026-10-28 |
| 2 | [`csharp`](examples/csharp) (the video's server) | — | C# | ASP.NET Core + Npgsql | 665,000 | 540,000 | 300,000 | 2026-10-19 |
| 3 | [`node`](examples/node) (the video's server) | — | Node | Fastify 5 + node-postgres, `node:cluster` workers | 587,000 | 310,000 | 125,000 | 2026-10-19 |
| 4 | [`rust`](examples/rust) (the video's server) | — | Rust | axum + SQLx | 413,000 | 227,000 | 114,000 | 2026-10-19 |
| 5 | [`java`](examples/java) (the video's server) | — | Java | Spring Boot 3.5 (Tomcat, virtual threads) + HikariCP + pgjdbc | 321,000 | 312,000 | 176,000 | 2026-10-19 |
| 6 | [`go`](examples/go) (the video's server) | — | Go | Gin + pgx/v5 `pgxpool` | 248,000 | 230,000 | 124,000 | 2026-10-19 |
| 7 | [`python`](examples/python) (the video's server) | — | Python | FastAPI on Gunicorn/Uvicorn workers + asyncpg | 213,000 | 104,000 | 51,000 | 2026-10-19 |

The six example servers were scored for the video (2026-10-18/19) with borrowed load generators and the old doubling
search; the test, the boxes and the pass rules are otherwise the same.
