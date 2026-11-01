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
| 1 | [`c-epoll-unorig-username`](submissions/c-epoll-unorig-username) | [#10](https://github.com/arjaythedev/million-users-challenge/pull/10) | C | own epoll event loop, hand-written HTTP/1.1, static libpq in pipeline mode, prepared statements, ISA-L gzip | 1,000,000 (max) | 1,420,000 | 1,370,000 | 2026-10-29 |
| 2 | [`fsharp-kestrel-uring-delneg`](submissions/fsharp-kestrel-uring-delneg) | [#35](https://github.com/arjaythedev/million-users-challenge/pull/35) | F# | io_uring thread per core with `SO_REUSEPORT` (multishot accept/recv, provided buffers) under Kestrel's host, hand-written HTTP/1.1 and Postgres wire client, separate read/write connections, ISA-L gzip | 1,000,000 (max) | 1,330,000 | 1,340,000 | 2026-11-01 |
| 3 | [`nelua-epoll-tbsvttr`](submissions/nelua-epoll-tbsvttr) | [#26](https://github.com/arjaythedev/million-users-challenge/pull/26) | Nelua | forked epoll worker per core with `SO_REUSEPORT`, hand-written HTTP/1.1 and pipelined Postgres wire client (libpq only logs in), separate read/write connections, ISA-L gzip | 1,000,000 (max) | 1,320,000 | 1,320,000 | 2026-11-01 |
| 4 | [`java-epoll-stohirov`](submissions/java-epoll-stohirov) | [#2](https://github.com/arjaythedev/million-users-challenge/pull/2) | Java | own epoll event loop per core (FFM), hand-written HTTP/1.1 and Postgres wire client, pipelined, group commit | 1,000,000 (max) | 1,310,000 | 1,260,000 | 2026-10-28 |
| 5 | [`zig-iouring-harshitkhandelwal208`](https://github.com/arjaythedev/million-users-challenge/pull/4)\* | [#4](https://github.com/arjaythedev/million-users-challenge/pull/4) | Zig | own io_uring HTTP/1.1 and Postgres wire client, own gzip, read packs + group-commit writes | 1,000,000 (max) | 1,270,000 | 1,320,000 | 2026-10-29 |
| 6 | [`cpp-io_uring1m-mattiasca06`](https://github.com/arjaythedev/million-users-challenge/pull/23)‡ | [#23](https://github.com/arjaythedev/million-users-challenge/pull/23) | C++ | own io_uring loop per core with `SO_REUSEPORT`, C++ coroutines, own Postgres wire client, pipelined prepared reads, group-commit writes | 1,000,000 (max) | 1,260,000 | 1,250,000 | 2026-10-31 |
| 7 | [`rust-tokio-unorig-username`](submissions/rust-tokio-unorig-username) | [#11](https://github.com/arjaythedev/million-users-challenge/pull/11) | Rust | current-thread tokio per core with `SO_REUSEPORT`, prepared statements, pipelined tokio-postgres | 1,000,000 (max) | 1,260,000 | 960,000 | 2026-10-29 |
| 8 | [`rust-uring-unorig-username`](submissions/rust-uring-unorig-username) | [#30](https://github.com/arjaythedev/million-users-challenge/pull/30) | Rust | thread per core on raw io_uring (multishot accept/recv, provided buffers), hand-written HTTP/1.1 and Postgres wire client, prepared statements | 1,000,000 (max) | 1,220,000 | 1,170,000 | 2026-10-31 |
| 9 | [`c-iouring-einhornimmond`](submissions/c-iouring-einhornimmond) | [#12](https://github.com/arjaythedev/million-users-challenge/pull/12) | C | own io_uring event loop (provided buffers, direct descriptors), libpq pipeline mode, ISA-L gzip | 1,000,000 (max) | 1,210,000 | 1,300,000 | 2026-10-29 |
| 10 | [`rust-tokio-cknutson12`](submissions/rust-tokio-cknutson12) | [#1](https://github.com/arjaythedev/million-users-challenge/pull/1) | Rust | thread-per-core tokio, hand-written HTTP/1.1, pipelined tokio-postgres | 1,000,000 (max) | 1,210,000 | 1,130,000 | 2026-10-28 |
| 11 | [`java-ffm-nnetraga97`](submissions/java-ffm-nnetraga97) | [#32](https://github.com/arjaythedev/million-users-challenge/pull/32) | Java | own epoll loop per core (FFM), hand-written HTTP/1.1 and Postgres wire client, separate read/write connections, libdeflate gzip via FFM | 1,000,000 (max) | 1,200,000 | 1,220,000 | 2026-10-31 |
| 12 | [`csharp-epoll-aot-gichamba`](submissions/csharp-epoll-aot-gichamba) | [#39](https://github.com/arjaythedev/million-users-challenge/pull/39) | C# | own epoll loops (NativeAOT, .NET 10), hand-written HTTP/1.1 and Postgres wire client, pipelined prepared reads, group-commit writes | 1,000,000 (max) | 1,180,000 | 1,220,000 | 2026-11-01 |
| 13 | [`c-h2o-einhornimmond`](submissions/c-h2o-einhornimmond) | [#9](https://github.com/arjaythedev/million-users-challenge/pull/9) | C | h2o (libh2o) built with Zig, libpq pipeline mode, prepared statements | 1,000,000 (max) | 1,180,000 | 1,160,000 | 2026-10-29 |
| 14 | [`csharp-epoll-xeonfx`](submissions/csharp-epoll-xeonfx) | [#31](https://github.com/arjaythedev/million-users-challenge/pull/31) | C# | own epoll loop per core with `SO_REUSEPORT` (NativeAOT, .NET 10), hand-written HTTP/1.1 and Postgres wire client, pipelined prepared reads, group-commit writes | 1,000,000 (max) | 1,080,000 | 1,240,000 | 2026-10-31 |
| 15 | [`typescript-bun-tbsvttr`](submissions/typescript-bun-tbsvttr) | [#13](https://github.com/arjaythedev/million-users-challenge/pull/13) | TypeScript | Bun.serve with `reusePort`, a worker per core, Bun.SQL prepared statements, libdeflate gzip | 1,000,000 (max) | 1,080,000 | 395,000 | 2026-10-29 |
| 16 | [`rust-tokio-dadujex`](submissions/rust-tokio-dadujex) | [#24](https://github.com/arjaythedev/million-users-challenge/pull/24) | Rust | thread-per-core current-thread tokio with `SO_REUSEPORT`, own HTTP/1.1 loop on httparse, prepared tokio-postgres with separate read/write connections | 1,000,000 (max) | 1,070,000 | 1,110,000 | 2026-10-31 |
| 17 | [`c-ammarserver-AmmarkoV`](submissions/c-ammarserver-AmmarkoV) | [#34](https://github.com/arjaythedev/million-users-challenge/pull/34) | C | AmmarServer in API mode on io_uring, 32 prefork workers with `SO_REUSEPORT`, batched libpq pipeline, prepared statements, libdeflate gzip | 1,000,000 (max) | 1,020,000 | 1,000,000 | 2026-11-01 |
| 18 | [`crystal-epoll-faustinoaq`](submissions/crystal-epoll-faustinoaq) | [#18](https://github.com/arjaythedev/million-users-challenge/pull/18) | Crystal | forked worker per core with `SO_REUSEPORT`, own io_uring loop and Postgres wire client, pipelined prepared statements, no GC | 1,000,000 (max) | 935,000 | 1,030,000 | 2026-10-31 |
| 19 | [`go-kamkm`](https://github.com/arjaythedev/million-users-challenge/pull/41)\*\* | [#41](https://github.com/arjaythedev/million-users-challenge/pull/41) | Go | gnet event loops with `SO_REUSEPORT` on both ports, net/http request parsing, pgx pool with pipelined reads (`pgx.Batch`), group-commit writes | 1,000,000 (max) | 815,000 | 440,000 | 2026-11-01 |
| 20 | [`c-iouring-TheFirstIstari`](https://github.com/arjaythedev/million-users-challenge/pull/16)† | [#16](https://github.com/arjaythedev/million-users-challenge/pull/16) | C | own io_uring/epoll event loop with `SO_REUSEPORT`, picohttpparser, libpq prepared statements, libdeflate gzip | 950,000 | 895,000 | 485,000 | 2026-10-31 |
| 21 | [`python-uvloop-shhossain`](submissions/python-uvloop-shhossain) | [#33](https://github.com/arjaythedev/million-users-challenge/pull/33) | Python | forked uvloop worker per core with `SO_REUSEPORT`, hand-written HTTP/1.1, asyncpg prepared statements, orjson, ISA-L gzip | 930,000 | 885,000 | 435,000 | 2026-11-01 |
| 22 | [`bun-elysia-youssefsz`](submissions/bun-elysia-youssefsz) | [#14](https://github.com/arjaythedev/million-users-challenge/pull/14) | TypeScript | Elysia 1.4 + Drizzle ORM over Bun SQL, prepared statements, admission queue | 840,000 | 500,000 | 225,000 | 2026-10-29 |
| 23 | [`java-quarkus-stohirov`](submissions/java-quarkus-stohirov) | [#3](https://github.com/arjaythedev/million-users-challenge/pull/3) | Java | Quarkus 3.40 (Vert.x Web router) + Vert.x reactive Postgres client | 780,000 | 605,000 | 440,000 | 2026-10-28 |
| 24 | [`csharp`](examples/csharp) (the video's server) | — | C# | ASP.NET Core + Npgsql | 665,000 | 540,000 | 300,000 | 2026-10-19 |
| 25 | [`elixir-bandit-tbsvttr`](submissions/elixir-bandit-tbsvttr) | [#15](https://github.com/arjaythedev/million-users-challenge/pull/15) | Elixir | Bandit 1.14 + bare Plug on OTP 29, Postgrex prepared statements | 650,000 | 315,000 | 129,000 | 2026-10-29 |
| 26 | [`csharp-kestrel-wylieagab`](submissions/csharp-kestrel-wylieagab) | [#28](https://github.com/arjaythedev/million-users-challenge/pull/28) | C# | Kestrel (NativeAOT, .NET 10) on both ports, Npgsql prepared statements, own JSON writer | 595,000 | 485,000 | 345,000 | 2026-10-31 |
| 27 | [`pony-stallion-tbsvttr`](submissions/pony-stallion-tbsvttr) | [#21](https://github.com/arjaythedev/million-users-challenge/pull/21) | Pony | Stallion HTTP server, an actor per connection, pure-Pony Postgres driver with prepared statements, libdeflate gzip | 590,000 | 300,000 | 131,000 | 2026-10-30 |
| 28 | [`node`](examples/node) (the video's server) | — | Node | Fastify 5 + node-postgres, `node:cluster` workers | 587,000 | 310,000 | 125,000 | 2026-10-19 |
| 29 | [`elixir-bandit-TheFirstIstari`](https://github.com/arjaythedev/million-users-challenge/pull/25)§ | [#25](https://github.com/arjaythedev/million-users-challenge/pull/25) | Elixir | Bandit 1.14 + Plug.Router, Postgrex prepared statements in pools sharded per scheduler | 580,000 | 285,000 | 120,000 | 2026-10-31 |
| 30 | [`forth-gforth-FantasticForth`](submissions/forth-gforth-FantasticForth) | [#8](https://github.com/arjaythedev/million-users-challenge/pull/8) | Forth | Gforth, forked epoll workers with `SO_REUSEPORT`, libpq prepared statements via FFI | 420,000 | 290,000 | 158,000 | 2026-10-29 |
| 31 | [`rust`](examples/rust) (the video's server) | — | Rust | axum + SQLx | 413,000 | 227,000 | 114,000 | 2026-10-19 |
| 32 | [`php-laravel-octane-gabidj`](https://github.com/arjaythedev/million-users-challenge/pull/38)‖ | [#38](https://github.com/arjaythedev/million-users-challenge/pull/38) | PHP | Laravel Octane on Swoole 6.2.4 (PHP 8.3), a server per port with a worker per core, coroutine PDO pool with prepared statements | 360,000 | 160,000 | 73,000 | 2026-11-01 |
| 33 | [`java`](examples/java) (the video's server) | — | Java | Spring Boot 3.5 (Tomcat, virtual threads) + HikariCP + pgjdbc | 321,000 | 312,000 | 176,000 | 2026-10-19 |
| 34 | [`go`](examples/go) (the video's server) | — | Go | Gin + pgx/v5 `pgxpool` | 248,000 | 230,000 | 124,000 | 2026-10-19 |
| 35 | [`python`](examples/python) (the video's server) | — | Python | FastAPI on Gunicorn/Uvicorn workers + asyncpg | 213,000 | 104,000 | 51,000 | 2026-10-19 |
| 36 | [`crystal-kemal-faustinoaq`](submissions/crystal-kemal-faustinoaq) | [#19](https://github.com/arjaythedev/million-users-challenge/pull/19) | Crystal | Kemal 1.15, a process per core with `SO_REUSEPORT`, crystal-pg | 194,000 | 180,000 | 103,000 | 2026-10-30 |
| 37 | [`scala-http4s-sideeffffect`](submissions/scala-http4s-sideeffffect) | [#7](https://github.com/arjaythedev/million-users-challenge/pull/7) | Scala | http4s Ember 0.23 on cats-effect 3 (JDK 27), Skunk session pools with prepared statements | 192,000 | 112,000 | 64,000 | 2026-11-01 |
| 38 | [`tex-cgi-Emsii3`](https://github.com/arjaythedev/million-users-challenge/pull/36)¶ | [#36](https://github.com/arjaythedev/million-users-challenge/pull/36) | TeX | C epoll front end on both ports feeding long-lived pdfTeX workers over FIFOs (JWT check and JSON in TeX), libpq async pool with prepared statements | 60,000 | 32,000 | 14,500 | 2026-11-01 |
| 39 | [`cmake-lots_of_things-IdkWhatToWriteHereOhManItWasAlreadyUsed`](https://github.com/arjaythedev/million-users-challenge/pull/17)ᵖ | [#17](https://github.com/arjaythedev/million-users-challenge/pull/17) | CMake | libevent listener + NASM launcher running `cmake -P` per request (HTTP, JWT, JSON in CMake script), libpq pool daemon over a unix socket | 1,000 | 1,000 | 1,000 | 2026-10-30 |

The six example servers were scored for the video (2026-10-18/19) with borrowed load generators and the old doubling
search; the test, the boxes and the pass rules are otherwise the same.

\* Held, not merged yet: #4 accepts a JWT with no `exp`, which the SPEC requires. Scored at 1f3ac0c on kernel
6.8.0-117 (its earlier `failed` was our app box's 6.8.0-147 kernel, not the entry).

† Held, not merged yet: #16 ships its libdeflate and picohttpparser sources as tarballs in `vendor/`, still to be
checked against upstream. Scored at e3595f8.

‡ Held, not merged yet: #23's review points are fixed, but CI hasn't run on the scored commit yet. Scored at 6a5de6a.

§ Held, not merged yet: #25's toolchain is now pinned (OTP 29.1.2 and Elixir 1.20.4, both SHA-256 checked), but CI
hasn't run on the scored commit yet. Scored at 2957066.

ᵖ Held, not merged yet: #17's `install.sh` was patched for the run only (it chowns `/app` to root, so `build.sh` as
`challenge` couldn't write there; the run used `chown -R challenge:challenge /app`), and it installs into `/app` and
`/tmp` instead of its own folder. Scored at 79c8ed4.

¶ Held, not merged yet: #36 keeps its per-request work files and FIFOs in `/dev/shm` (or `$TMPDIR`, else `/tmp`)
instead of its own folder. Scored at 8b8dc81.

\*\* Held, not merged yet: #41's `start.sh` reads the box's `/proc/meminfo` (to size `GOMEMLIMIT`), outside its
folder, and CI hasn't run on the scored commit yet. Scored at 8dc228e.

‖ Held, not merged yet: #38 accepts a JWT with no `exp`, which the SPEC requires. Scored at 525d311.
