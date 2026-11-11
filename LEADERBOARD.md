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
by 4 cores.

| # | Submission | PR | Language | Stack | 16 cores | 8 cores | 4 cores | Scored |
|---:|---|---|---|---|---:|---:|---:|---|
| 1 | [`fsharp-kestrel-uring-delneg`](submissions/fsharp-kestrel-uring-delneg) | [#35](https://github.com/arjaythedev/million-users-challenge/pull/35), [#105](https://github.com/arjaythedev/million-users-challenge/pull/105), [#108](https://github.com/arjaythedev/million-users-challenge/pull/108), [#109](https://github.com/arjaythedev/million-users-challenge/pull/109), [#110](https://github.com/arjaythedev/million-users-challenge/pull/110) | F# | io_uring thread per core with `SO_REUSEPORT` (multishot accept/recv, provided buffers) in a plain .NET console app, hand-written HTTP/1.1 and Postgres wire client, separate read/write connections, ISA-L gzip with a Huffman table retrained from live bodies | 1,000,000 (max) | 1,420,000 | 1,510,000 | 2026-11-10 |
| 2 | [`bend-epoll-wyrmkeep`](https://github.com/arjaythedev/million-users-challenge/pull/113)\*\*\*\*\*\* | [#113](https://github.com/arjaythedev/million-users-challenge/pull/113) | Bend | Bend 2 request logic (compiled to C, with proofs) under a C epoll host, one single-threaded process per CPU with `SO_REUSEPORT` on both ports, hand-written HTTP/1.1 and Postgres wire client (SCRAM, binary rows), prepared reads batched under one Sync, group-commit writes on separate connections answered at `ReadyForQuery`, newest-first backlog order, ISA-L gzip, PGO build | 1,000,000 (max) | 1,480,000 | 1,480,000 | 2026-11-11 |
| 3 | [`c-epoll-unorig-username`](submissions/c-epoll-unorig-username) | [#10](https://github.com/arjaythedev/million-users-challenge/pull/10), [#90](https://github.com/arjaythedev/million-users-challenge/pull/90), [#96](https://github.com/arjaythedev/million-users-challenge/pull/96), [#104](https://github.com/arjaythedev/million-users-challenge/pull/104), [#100](https://github.com/arjaythedev/million-users-challenge/pull/100) | C | own epoll loop per core, hand-written HTTP/1.1 and Postgres wire client (SCRAM, binary rows), prepared statements, group-commit writes answered at `ReadyForQuery`, `TCP_DEFER_ACCEPT` listeners, ISA-L gzip | 1,000,000 (max) | 1,420,000 | 1,480,000 | 2026-11-11 |
| 4 | [`csharp-epoll-xeonfx`](submissions/csharp-epoll-xeonfx) | [#31](https://github.com/arjaythedev/million-users-challenge/pull/31), [#77](https://github.com/arjaythedev/million-users-challenge/pull/77), [#111](https://github.com/arjaythedev/million-users-challenge/pull/111), [#114](https://github.com/arjaythedev/million-users-challenge/pull/114) | C# | own epoll loop per core with `SO_REUSEPORT` (NativeAOT, .NET 10), hand-written HTTP/1.1 and Postgres wire client, one read batch per 2 ms loop pass under one Sync, group-commit writes answered at `ReadyForQuery`, `TCP_DEFER_ACCEPT` listeners, own gzip encoder | 1,000,000 (max) | 1,460,000 | 1,460,000 | 2026-11-11 |
| 5 | [`swift-iouring-akbashev`](https://github.com/arjaythedev/million-users-challenge/pull/112)‖‖‖‖‖‖ | [#112](https://github.com/arjaythedev/million-users-challenge/pull/112) | Swift | Swift 6.4 actor per core whose executor is its own io_uring loop (raw syscalls, no liburing; multishot accept/recv, provided buffers), `SO_REUSEPORT` on both ports, hand-written HTTP/1.1 and Postgres wire client, prepared reads sharing one Sync per turn, group-commit writes on separate connections answered at `ReadyForQuery`, own gzip | 1,000,000 (max) | 1,420,000 | 1,460,000 | 2026-11-11 |
| 6 | [`csharp-epoll-jh-d`](submissions/csharp-epoll-jh-d) | [#59](https://github.com/arjaythedev/million-users-challenge/pull/59) | C# | own epoll loop per core with `SO_REUSEPORT` on both ports (.NET 10, JIT), raw fds, hand-written HTTP/1.1 and Postgres wire client, prepared reads batched under one Sync, separate write connections answered after commit | 1,000,000 (max) | 1,360,000 | 1,430,000 | 2026-11-03 |
| 7 | [`go-epoll-trungdlp`](https://github.com/arjaythedev/million-users-challenge/pull/65)††† | [#65](https://github.com/arjaythedev/million-users-challenge/pull/65) | Go | own epoll loop per core with `SO_REUSEPORT` on both ports, hand-written HTTP/1.1 and Postgres wire client (pgconn only connects and prepares), reads batched under one Sync, group-commit writes on separate connections answered at `ReadyForQuery`, `TCP_DEFER_ACCEPT` listeners, gzip Huffman tables rebuilt from live bodies | 1,000,000 (max) | 1,330,000 | 1,430,000 | 2026-11-10 |
| 8 | [`c-ammarserver-AmmarkoV`](https://github.com/arjaythedev/million-users-challenge/pull/116)††††††† | [#34](https://github.com/arjaythedev/million-users-challenge/pull/34), [#80](https://github.com/arjaythedev/million-users-challenge/pull/80), [#84](https://github.com/arjaythedev/million-users-challenge/pull/84), [#92](https://github.com/arjaythedev/million-users-challenge/pull/92), [#116](https://github.com/arjaythedev/million-users-challenge/pull/116) | C | AmmarServer in API mode on io_uring, a prefork worker per CPU with `SO_REUSEPORT` on both ports, one ring thread per worker that also drives its Postgres connections (libpq only logs in and prepares, then its own wire code: one Bind/Execute per request), a 2 ms pass's reads sharing one Sync, group-commit writes answered at `ReadyForQuery`, hand-written JSON, ISA-L gzip | 1,000,000 (max) | 1,430,000 | 1,420,000 | 2026-11-11 |
| 9 | [`swift-epoll-akbashev`](https://github.com/arjaythedev/million-users-challenge/pull/83)‖‖‖‖ | [#83](https://github.com/arjaythedev/million-users-challenge/pull/83) | Swift | Swift 6.4 actor per core whose executor is its own epoll loop, `SO_REUSEPORT` on both ports, hand-written HTTP/1.1 and Postgres wire client, prepared reads sharing one Sync per turn, group-commit writes on separate connections answered at `ReadyForQuery`, own gzip | 1,000,000 (max) | 1,360,000 | 1,410,000 | 2026-11-11 |
| 10 | [`rust-tokio-meowcakes`](https://github.com/arjaythedev/million-users-challenge/pull/88)\*\*\*\*\* | [#88](https://github.com/arjaythedev/million-users-challenge/pull/88) | Rust | thread per core on raw io_uring with `SO_REUSEPORT` on both ports, hand-written HTTP/1.1 and Postgres wire client, prepared reads batched under one Sync, group-commit writes on separate write lanes answered at `ReadyForQuery`, own gzip | 1,000,000 (max) | 1,440,000 | 1,390,000 | 2026-11-06 |
| 11 | [`java-epoll-stohirov`](submissions/java-epoll-stohirov) | [#2](https://github.com/arjaythedev/million-users-challenge/pull/2), [#78](https://github.com/arjaythedev/million-users-challenge/pull/78) | Java | own epoll event loop per core (FFM), hand-written HTTP/1.1 and Postgres wire client, reads pipelined under one Sync, group-commit writes on separate connections, own gzip | 1,000,000 (max) | 1,390,000 | 1,390,000 | 2026-11-04 |
| 12 | [`python-hexagonal-shawal-mbalire`](https://github.com/arjaythedev/million-users-challenge/pull/66)¶¶¶¶¶ | [#66](https://github.com/arjaythedev/million-users-challenge/pull/66) | Python | ports-and-adapters design; forked worker per core with `SO_REUSEPORT` on both ports: a C epoll loop serves `GET /feed` and `GET /posts/:id` (prepared reads sharing one Sync per pass), Python (uvloop) handles JWT, writes (group commit, answered after commit) and errors; static libpq 18 in pipeline mode via a C extension, C JSON encoder, ISA-L gzip | 1,000,000 (max) | 1,360,000 | 1,390,000 | 2026-11-07 |
| 13 | [`go-nuclear-reactor-praveen-propip`](https://github.com/arjaythedev/million-users-challenge/pull/74)‖‖‖ | [#74](https://github.com/arjaythedev/million-users-challenge/pull/74) | Go | forked `GOMAXPROCS=1` worker per core with `SO_REUSEPORT` on both ports, own epoll loop (optional io_uring), hand-written HTTP/1.1 and Postgres wire client, reads pipelined under one Sync, group-commit writes answered after commit | 1,000,000 (max) | 1,320,000 | 1,370,000 | 2026-11-03 |
| 14 | [`swift-iouring-Tabonx`](https://github.com/arjaythedev/million-users-challenge/pull/85)\*\*\*\* | [#85](https://github.com/arjaythedev/million-users-challenge/pull/85) | Swift | io_uring worker thread per core on liburing (direct descriptors, provided buffers), each with its own listening sockets on both ports, hand-written HTTP/1.1 and Postgres wire client (SCRAM, binary rows), prepared reads packed under one Sync, separate write connections answered at `ReadyForQuery`, ISA-L gzip | 1,000,000 (max) | 1,360,000 | 1,360,000 | 2026-11-04 |
| 15 | [`go-kamkm`](https://github.com/arjaythedev/million-users-challenge/pull/41)\*\* | [#41](https://github.com/arjaythedev/million-users-challenge/pull/41) | Go | gnet event loops with `SO_REUSEPORT` on both ports, net/http request parsing, pgx pool with 48 pipelined read workers (`pgx.Batch`) and a fixed responder pool, group-commit write lanes, own fixed-Huffman gzip | 1,000,000 (max) | 1,250,000 | 1,360,000 | 2026-11-07 |
| 16 | [`goose-epoll-api-bhare1985`](https://github.com/arjaythedev/million-users-challenge/pull/76)†††† | [#76](https://github.com/arjaythedev/million-users-challenge/pull/76) | Goose | Goose (compiles to C) engine driven by its `openapi.json` (routes, SQL, templates), epoll loop per core, hand-written HTTP/1.1, static libpq 18.6 in pipeline mode with prepared statements, reads under one Sync, writes answered at their Sync, ISA-L gzip | 1,000,000 (max) | 1,350,000 | 1,350,000 | 2026-11-05 |
| 17 | [`zig-iouring-harshitkhandelwal208`](https://github.com/arjaythedev/million-users-challenge/pull/4)\* | [#4](https://github.com/arjaythedev/million-users-challenge/pull/4) | Zig | own io_uring HTTP/1.1 and Postgres wire client, a worker per available core, own gzip, read packs + group-commit writes | 1,000,000 (max) | 1,340,000 | 1,350,000 | 2026-11-06 |
| 18 | [`rust-uring-unorig-username`](submissions/rust-uring-unorig-username) | [#30](https://github.com/arjaythedev/million-users-challenge/pull/30), [#73](https://github.com/arjaythedev/million-users-challenge/pull/73), [#97](https://github.com/arjaythedev/million-users-challenge/pull/97) | Rust | thread per core on raw io_uring (multishot accept/recv, provided buffers), hand-written HTTP/1.1 and Postgres wire client, prepared statements, reads packed under one Sync, group-commit writes answered at `ReadyForQuery` | 1,000,000 (max) | 1,360,000 | 1,340,000 | 2026-11-06 |
| 19 | [`asm-epoll-tobycat`](https://github.com/arjaythedev/million-users-challenge/pull/99)‡‡‡‡‡‡ | [#99](https://github.com/arjaythedev/million-users-challenge/pull/99) | x86-64 assembly | static binary, no libc: raw syscalls, epoll loop per CPU with `SO_REUSEPORT` on both ports, hand-written HTTP/1.1, JSON, HS256, gzip and Postgres wire client (SCRAM, binary rows), prepared reads sharing one Sync per batch, group-commit writes on one write connection answered at `ReadyForQuery` | 1,000,000 (max) | 1,390,000 | 1,330,000 | 2026-11-10 |
| 20 | [`lisp-sbcl-tobycat`](https://github.com/arjaythedev/million-users-challenge/pull/106)¶¶¶¶¶¶ | [#106](https://github.com/arjaythedev/million-users-challenge/pull/106) | Common Lisp | SBCL, forked worker per CPU with `SO_REUSEPORT` on both ports, own epoll loop, hand-written HTTP/1.1, JSON, HS256, gzip and Postgres wire client (SCRAM), prepared statements, group-commit writes answered at `ReadyForQuery` | 1,000,000 (max) | 1,300,000 | 1,330,000 | 2026-11-10 |
| 21 | [`nelua-epoll-tbsvttr`](submissions/nelua-epoll-tbsvttr) | [#26](https://github.com/arjaythedev/million-users-challenge/pull/26) | Nelua | forked epoll worker per core with `SO_REUSEPORT`, hand-written HTTP/1.1 and pipelined Postgres wire client (libpq only logs in), separate read/write connections, ISA-L gzip | 1,000,000 (max) | 1,320,000 | 1,320,000 | 2026-11-01 |
| 22 | [`c-iouring-PhilipNsuna`](https://github.com/arjaythedev/million-users-challenge/pull/60)‡‡‡‡‡ | [#60](https://github.com/arjaythedev/million-users-challenge/pull/60) | C | io_uring loop per core on liburing (multishot accept/recv, provided buffers, epoll fallback) with `SO_REUSEPORT` on both ports, hand-written HTTP/1.1 and Postgres wire client (SCRAM, binary rows), pipelined prepared statements on separate read/write connections, writes answered at `ReadyForQuery`, own gzip encoder | 1,000,000 (max) | 1,270,000 | 1,320,000 | 2026-11-09 |
| 23 | [`nish-std-amritk`](submissions/nish-std-amritk) | [#56](https://github.com/arjaythedev/million-users-challenge/pull/56), [#107](https://github.com/arjaythedev/million-users-challenge/pull/107) | Nish | the author's TypeScript-subset AOT compiler (LLVM): prefork epoll worker per core with `SO_REUSEPORT` on both ports, hand-written HTTP/1.1 and Postgres wire client, prepared reads batched under one Sync, group-commit writes on separate connections answered at `ReadyForQuery` | 1,000,000 (max) | 1,200,000 | 1,320,000 | 2026-11-09 |
| 24 | [`cpp-epoll-dungngo4520`](https://github.com/arjaythedev/million-users-challenge/pull/37)†† | [#37](https://github.com/arjaythedev/million-users-challenge/pull/37) | C++ | own epoll loop per thread with `SO_REUSEPORT`, hand-written HTTP/1.1 and Postgres wire client, prepared reads batched under one Sync, write-preferred connections (512 in total; when the pool is full, requests queue for the next free connection instead of failing), libdeflate gzip | 1,000,000 (max) | 1,310,000 | 1,310,000 | 2026-11-08 |
| 25 | [`c3-epoll-nghiaphamln`](https://github.com/arjaythedev/million-users-challenge/pull/82)¶¶¶¶ | [#82](https://github.com/arjaythedev/million-users-challenge/pull/82) | C3 | own epoll loop per core with `SO_REUSEPORT` on both ports, hand-written HTTP/1.1 and Postgres wire client, prepared reads sharing one Sync per loop pass, group-commit writes answered at `ReadyForQuery`, ISA-L gzip | 1,000,000 (max) | 1,360,000 | 1,300,000 | 2026-11-07 |
| 26 | [`kotlin-native-luben93`](https://github.com/arjaythedev/million-users-challenge/pull/47)‖‖ | [#47](https://github.com/arjaythedev/million-users-challenge/pull/47) | Kotlin | Kotlin/Native, own epoll loop per core with `SO_REUSEPORT` on both ports, hand-written HTTP/1.1 and Postgres wire client, pipelined prepared reads, group-commit writes | 1,000,000 (max) | 1,160,000 | 1,290,000 | 2026-11-05 |
| 27 | [`asm-epoll-nickita-khylkouski`](https://github.com/arjaythedev/million-users-challenge/pull/61)§§§§§ | [#61](https://github.com/arjaythedev/million-users-challenge/pull/61) | x86-64 assembly | static binary, no libc: raw syscalls, epoll loop per core with `SO_REUSEPORT` on both ports, hand-written HTTP/1.1, JSON, HMAC-SHA256, gzip and Postgres wire client (SCRAM, pipelined prepared statements), writes answered at `ReadyForQuery` | 1,000,000 (max) | 1,330,000 | 1,280,000 | 2026-11-05 |
| 28 | [`rust-kamkm`](https://github.com/arjaythedev/million-users-challenge/pull/42)‡‡ | [#42](https://github.com/arjaythedev/million-users-challenge/pull/42) | Rust | multi-thread tokio with its own HTTP/1.1 transport on httparse, deadpool-postgres with pipelined prepared reads, group-commit writes answered after commit, JSON written from row values, own fixed-Huffman gzip, mimalloc | 1,000,000 (max) | 1,250,000 | 1,280,000 | 2026-11-07 |
| 29 | [`go-epoll-kaikegm`](https://github.com/arjaythedev/million-users-challenge/pull/27)††††† | [#27](https://github.com/arjaythedev/million-users-challenge/pull/27) | Go | own level-triggered epoll loop per `GOMAXPROCS` thread with `SO_REUSEPORT` on both ports, hand-written HTTP/1.1 and Postgres wire client (pgconn only connects), prepared reads and writes pipelined on separate read/write connections, writes answered at `ReadyForQuery`, zerojwt HS256, own fixed-Huffman gzip | 1,000,000 (max) | 1,300,000 | 1,260,000 | 2026-11-08 |
| 30 | [`cpp-io_uring1m-mattiasca06`](https://github.com/arjaythedev/million-users-challenge/pull/23)‡ | [#23](https://github.com/arjaythedev/million-users-challenge/pull/23) | C++ | own io_uring loop per core with `SO_REUSEPORT`, C++ coroutines, own Postgres wire client, pipelined prepared reads, group-commit writes | 1,000,000 (max) | 1,260,000 | 1,250,000 | 2026-10-31 |
| 31 | [`java-ffm-nnetraga97`](submissions/java-ffm-nnetraga97) | [#32](https://github.com/arjaythedev/million-users-challenge/pull/32) | Java | own epoll loop per core (FFM), hand-written HTTP/1.1 and Postgres wire client, separate read/write connections, libdeflate gzip via FFM | 1,000,000 (max) | 1,200,000 | 1,220,000 | 2026-10-31 |
| 32 | [`csharp-epoll-aot-gichamba`](submissions/csharp-epoll-aot-gichamba) | [#39](https://github.com/arjaythedev/million-users-challenge/pull/39) | C# | own epoll loops (NativeAOT, .NET 10), hand-written HTTP/1.1 and Postgres wire client, pipelined prepared reads, group-commit writes | 1,000,000 (max) | 1,180,000 | 1,220,000 | 2026-11-01 |
| 33 | [`c-iouring-einhornimmond`](submissions/c-iouring-einhornimmond) | [#12](https://github.com/arjaythedev/million-users-challenge/pull/12), [#63](https://github.com/arjaythedev/million-users-challenge/pull/63) | C | own io_uring event loop (provided buffers, direct descriptors), libpq pipeline mode, ISA-L gzip | 1,000,000 (max) | 1,180,000 | 1,210,000 | 2026-11-03 |
| 34 | [`zig-kamkm`](https://github.com/arjaythedev/million-users-challenge/pull/43)§§ | [#43](https://github.com/arjaythedev/million-users-challenge/pull/43) | Zig | two `SO_REUSEPORT` epoll loops, hand-written HTTP/1.1, static libpq workers with prepared statements, pipelined reads under one Sync, group-commit writes, ISA-L gzip at level 0 | 1,000,000 (max) | 1,280,000 | 1,190,000 | 2026-11-09 |
| 35 | [`c-h2o-einhornimmond`](submissions/c-h2o-einhornimmond) | [#9](https://github.com/arjaythedev/million-users-challenge/pull/9) | C | h2o (libh2o) built with Zig, libpq pipeline mode, prepared statements | 1,000,000 (max) | 1,180,000 | 1,160,000 | 2026-10-29 |
| 36 | [`rust-tokio-cknutson12`](submissions/rust-tokio-cknutson12) | [#1](https://github.com/arjaythedev/million-users-challenge/pull/1) | Rust | thread-per-core tokio, hand-written HTTP/1.1, pipelined tokio-postgres | 1,000,000 (max) | 1,210,000 | 1,130,000 | 2026-10-28 |
| 37 | [`c-iouring-alexandretoscano04`](submissions/c-iouring-alexandretoscano04) | [#50](https://github.com/arjaythedev/million-users-challenge/pull/50) | C | forked worker per core with `SO_REUSEPORT` on both ports, raw io_uring (provided buffers, no liburing), own Postgres wire client (libpq only logs in), pipelined prepared reads under one Sync, group-commit writes | 1,000,000 (max) | 1,100,000 | 1,120,000 | 2026-11-02 |
| 38 | [`crystal-rayo-faustinoaq`](submissions/crystal-rayo-faustinoaq) | [#49](https://github.com/arjaythedev/million-users-challenge/pull/49) | Crystal | the author's Rayo framework: forked worker per core with `SO_REUSEPORT` on both ports, io_uring loop, Rayo::PG pipelined prepared statements, libdeflate gzip, no GC | 1,000,000 (max) | 1,060,000 | 1,120,000 | 2026-11-02 |
| 39 | [`rust-tokio-dadujex`](submissions/rust-tokio-dadujex) | [#24](https://github.com/arjaythedev/million-users-challenge/pull/24) | Rust | thread-per-core current-thread tokio with `SO_REUSEPORT`, own HTTP/1.1 loop on httparse, prepared tokio-postgres with separate read/write connections | 1,000,000 (max) | 1,070,000 | 1,110,000 | 2026-10-31 |
| 40 | [`java-epoll-buianhtai`](submissions/java-epoll-buianhtai) | [#45](https://github.com/arjaythedev/million-users-challenge/pull/45) | Java | own epoll loop per core (FFM) with `SO_REUSEPORT`, hand-written HTTP/1.1 and Postgres wire client (SCRAM), pipelined prepared reads, group-commit writes, JDK gzip | 1,000,000 (max) | 1,190,000 | 1,090,000 | 2026-11-02 |
| 41 | [`crystal-epoll-faustinoaq`](submissions/crystal-epoll-faustinoaq) | [#18](https://github.com/arjaythedev/million-users-challenge/pull/18) | Crystal | forked worker per core with `SO_REUSEPORT`, own io_uring loop and Postgres wire client, pipelined prepared statements, no GC | 1,000,000 (max) | 935,000 | 1,030,000 | 2026-10-31 |
| 42 | [`cpp-iouring_eastl-hendy643`](https://github.com/arjaythedev/million-users-challenge/pull/64)ᵖᵖ | [#64](https://github.com/arjaythedev/million-users-challenge/pull/64) | C++ | C++23 io_uring loop per core with `SO_REUSEPORT` (multishot accept/recv, provided buffers), EASTL + StringZilla, hand-written Postgres wire client (SCRAM, binary rows), prepared statements pipelined under one Sync per loop iteration, libdeflate gzip, PGO build | 1,000,000 (max) | 1,150,000 | 1,010,000 | 2026-11-06 |
| 43 | [`cpp-iouring-theargcoder`](https://github.com/arjaythedev/million-users-challenge/pull/94)§§§§§§ | [#94](https://github.com/arjaythedev/million-users-challenge/pull/94) | C++ | io_uring accept/recv (multishot, provided buffers) feeding 48–192 HTTP worker threads (by core count), each with its own lock-free SPSC queue and libpqxx connection, prepared statements pipelined in one transaction per batch | 895,000 | 980,000 | 970,000 | 2026-11-10 |
| 44 | [`rust-hyper-jgaalen`](submissions/rust-hyper-jgaalen) | [#52](https://github.com/arjaythedev/million-users-challenge/pull/52) | Rust | current-thread tokio per core fed by one acceptor on both ports, hand-written HTTP/1.1 and Postgres wire client, prepared statements, batched writes answered after `COMMIT` | 1,000,000 (max) | 945,000 | 970,000 | 2026-11-02 |
| 45 | [`rust-tokio-unorig-username`](submissions/rust-tokio-unorig-username) | [#11](https://github.com/arjaythedev/million-users-challenge/pull/11) | Rust | current-thread tokio per core with `SO_REUSEPORT`, prepared statements, pipelined tokio-postgres | 1,000,000 (max) | 1,260,000 | 960,000 | 2026-10-29 |
| 46 | [`java-micronaut-Clickin`](submissions/java-micronaut-Clickin) | [#93](https://github.com/arjaythedev/million-users-challenge/pull/93) | Java | Micronaut 5.2 HTTP server (Netty, native epoll, an event loop per core) on both ports, a Vert.x reactive Postgres pool per event loop with pipelined prepared statements, Temurin 26 | 1,000,000 (max) | 1,180,000 | 890,000 | 2026-11-06 |
| 47 | [`java-vertx-Clickin`](submissions/java-vertx-Clickin) | [#58](https://github.com/arjaythedev/million-users-challenge/pull/58) | Java | Vert.x 5 core `HttpServer`, a verticle per event loop on both ports, Vert.x reactive Postgres client with prepared statements, Temurin 26 | 1,000,000 (max) | 1,110,000 | 855,000 | 2026-11-03 |
| 48 | [`dexlang-iouring-ensamuel7`](https://github.com/arjaythedev/million-users-challenge/pull/69)§§§ | [#69](https://github.com/arjaythedev/million-users-challenge/pull/69) | DexLang | the author's own language: forked io_uring workers (4 per core) with `SO_REUSEPORT` on both ports, one Postgres connection per worker with prepared statements sent without waiting and answered in order after each batch, libdeflate gzip | 1,000,000 (max) | 1,120,000 | 795,000 | 2026-11-10 |
| 49 | [`csharp-epoll-wylieagab`](submissions/csharp-epoll-wylieagab) | [#72](https://github.com/arjaythedev/million-users-challenge/pull/72) | C# | worker thread per core (.NET 10, JIT) with its own epoll loop and `SO_REUSEPORT` on both ports, hand-written HTTP/1.1, libpq 18.6 (built in its folder) in nonblocking pipeline mode with prepared statements, separate read/write connections, writes answered after their Sync | 1,000,000 (max) | 910,000 | 765,000 | 2026-11-05 |
| 50 | [`ocaml-httpuneio-art-w`](https://github.com/arjaythedev/million-users-challenge/pull/71)‖‖‖‖‖ | [#71](https://github.com/arjaythedev/million-users-challenge/pull/71) | OCaml | OCaml 5.5 multicore: an Eio domain per core (io_uring) with `SO_REUSEPORT` on both ports, httpun driven by one fiber per connection, own pipelined Postgres wire client (SCRAM, prepared statements, reads sharing one Sync, each write answered after its commit), ISA-L gzip | 1,000,000 (max) | 1,070,000 | 730,000 | 2026-11-05 |
| 51 | [`c-iouring-TheFirstIstari`](https://github.com/arjaythedev/million-users-challenge/pull/16)† | [#16](https://github.com/arjaythedev/million-users-challenge/pull/16) | C | own io_uring/epoll event loop with `SO_REUSEPORT`, picohttpparser, libpq prepared statements, libdeflate gzip | 950,000 | 840,000 | 575,000 | 2026-11-07 |
| 52 | [`basic-freebasic-jgaalen`](submissions/basic-freebasic-jgaalen) | [#54](https://github.com/arjaythedev/million-users-challenge/pull/54) | FreeBASIC | forked libevent worker per core with `SO_REUSEPORT` on both ports, hand-written HTTP/1.1 and JSON in BASIC, async libpq pools with prepared statements, replies after ReadyForQuery, no gzip | 950,000 | 890,000 | 525,000 | 2026-11-02 |
| 53 | [`fsharp-aspnetcore-delneg`](https://github.com/arjaythedev/million-users-challenge/pull/67)‡‡‡ | [#67](https://github.com/arjaythedev/million-users-challenge/pull/67) | F# | stock ASP.NET Core (Kestrel, .NET 11 RC) on both ports, Npgsql with multiplexing and auto-prepared statements, hand-written HS256 check | 1,000,000 (max) | 980,000 | 465,000 | 2026-11-03 |
| 54 | [`go-hertz-molarom`](https://github.com/arjaythedev/million-users-challenge/pull/81)§§§§ | [#81](https://github.com/arjaythedev/million-users-challenge/pull/81) | Go | CloudWeGo Hertz on netpoll on both ports, pgx with prepared statements, reads pipelined with a Sync per query, group-commit writes answered after `ReadyForQuery` | 1,000,000 (max) | 750,000 | 440,000 | 2026-11-04 |
| 55 | [`java-quarkus-stohirov`](submissions/java-quarkus-stohirov) | [#3](https://github.com/arjaythedev/million-users-challenge/pull/3) | Java | Quarkus 3.40 (Vert.x Web router) + Vert.x reactive Postgres client | 780,000 | 605,000 | 440,000 | 2026-10-28 |
| 56 | [`python-uvloop-shhossain`](submissions/python-uvloop-shhossain) | [#33](https://github.com/arjaythedev/million-users-challenge/pull/33) | Python | forked uvloop worker per core with `SO_REUSEPORT`, hand-written HTTP/1.1, asyncpg prepared statements, orjson, ISA-L gzip | 930,000 | 885,000 | 435,000 | 2026-11-01 |
| 57 | [`typescript-bun-tbsvttr`](submissions/typescript-bun-tbsvttr) | [#13](https://github.com/arjaythedev/million-users-challenge/pull/13) | TypeScript | Bun.serve with `reusePort`, a worker per core, Bun.SQL prepared statements, libdeflate gzip | 1,000,000 (max) | 1,080,000 | 395,000 | 2026-10-29 |
| 58 | [`java-quarkus-deviknitkkr`](https://github.com/arjaythedev/million-users-challenge/pull/44)¶¶ | [#44](https://github.com/arjaythedev/million-users-challenge/pull/44) | Java | Quarkus 3.40 REST + Mutiny, Vert.x reactive Postgres client with pipelined prepared queries, a second listener on `EXTRA_PORT` | 1,000,000 (max) | 745,000 | 365,000 | 2026-11-02 |
| 59 | [`csharp-kestrel-wylieagab`](submissions/csharp-kestrel-wylieagab) | [#28](https://github.com/arjaythedev/million-users-challenge/pull/28) | C# | Kestrel (NativeAOT, .NET 10) on both ports, Npgsql prepared statements, own JSON writer | 595,000 | 485,000 | 345,000 | 2026-10-31 |
| 60 | [`csharp`](examples/csharp) (the video's server) | — | C# | ASP.NET Core + Npgsql | 665,000 | 540,000 | 300,000 | 2026-10-19 |
| 61 | [`go-nethttp-praveen-propip`](https://github.com/arjaythedev/million-users-challenge/pull/75)\*\*\* | [#75](https://github.com/arjaythedev/million-users-challenge/pull/75) | Go | stdlib `net/http` on both ports, gzhttp, pgx `pgxpool` (256 connections) with cached prepared statements | 675,000 | 495,000 | 265,000 | 2026-11-03 |
| 62 | [`ruby-iodine-Alex-Bradshaw`](https://github.com/arjaythedev/million-users-challenge/pull/70)¶¶¶ | [#70](https://github.com/arjaythedev/million-users-challenge/pull/70) | Ruby | Iodine 0.8 (facil.io) workers with 6 Ruby threads per core on Ruby 4.0 + YJIT, one `pg` connection per thread with prepared statements, on both ports | 550,000 | 510,000 | 235,000 | 2026-11-03 |
| 63 | [`bun-elysia-youssefsz`](submissions/bun-elysia-youssefsz) | [#14](https://github.com/arjaythedev/million-users-challenge/pull/14) | TypeScript | Elysia 1.4 + Drizzle ORM over Bun SQL, prepared statements, admission queue | 840,000 | 500,000 | 225,000 | 2026-10-29 |
| 64 | [`python-fastapi-tejasnafde`](submissions/python-fastapi-tejasnafde) | [#46](https://github.com/arjaythedev/million-users-challenge/pull/46) | Python | FastAPI on Gunicorn with uvicorn workers (uvloop, httptools), asyncpg prepared statements in a per-worker pool, `SO_REUSEPORT` on both ports | 715,000 | 395,000 | 215,000 | 2026-11-02 |
| 65 | [`java-springboot-danieloyoo`](https://github.com/arjaythedev/million-users-challenge/pull/86)†††††† | [#86](https://github.com/arjaythedev/million-users-challenge/pull/86) | Java | Spring Boot 3.2 (Tomcat, Spring MVC, virtual threads) on both ports, `JdbcTemplate` over HikariCP (128 connections) and pgjdbc, Auth0 `java-jwt` | 340,000 | 390,000 | 200,000 | 2026-11-10 |
| 66 | [`java`](examples/java) (the video's server) | — | Java | Spring Boot 3.5 (Tomcat, virtual threads) + HikariCP + pgjdbc | 321,000 | 312,000 | 176,000 | 2026-10-19 |
| 67 | [`forth-gforth-FantasticForth`](submissions/forth-gforth-FantasticForth) | [#8](https://github.com/arjaythedev/million-users-challenge/pull/8) | Forth | Gforth, forked epoll workers with `SO_REUSEPORT`, libpq prepared statements via FFI | 420,000 | 290,000 | 158,000 | 2026-10-29 |
| 68 | [`erlang-gen_tcp-quefep-bmgo`](submissions/erlang-gen_tcp-quefep-bmgo) | [#48](https://github.com/arjaythedev/million-users-challenge/pull/48) | Erlang | `gen_tcp` acceptors on both ports, a process per connection, hand-written HTTP/1.1, epgsql prepared statements over round-robin connections, jsone | 820,000 | 385,000 | 156,000 | 2026-11-02 |
| 69 | [`elixir-bandit-TheFirstIstari`](https://github.com/arjaythedev/million-users-challenge/pull/25)§ | [#25](https://github.com/arjaythedev/million-users-challenge/pull/25) | Elixir | Bandit 1.14 + Plug.Router, Postgrex prepared statements in pools sharded per scheduler | 620,000 | 300,000 | 133,000 | 2026-11-07 |
| 70 | [`pony-stallion-tbsvttr`](submissions/pony-stallion-tbsvttr) | [#21](https://github.com/arjaythedev/million-users-challenge/pull/21) | Pony | Stallion HTTP server, an actor per connection, pure-Pony Postgres driver with prepared statements, libdeflate gzip | 590,000 | 300,000 | 131,000 | 2026-10-30 |
| 71 | [`elixir-bandit-tbsvttr`](submissions/elixir-bandit-tbsvttr) | [#15](https://github.com/arjaythedev/million-users-challenge/pull/15) | Elixir | Bandit 1.14 + bare Plug on OTP 29, Postgrex prepared statements | 650,000 | 315,000 | 129,000 | 2026-10-29 |
| 72 | [`node`](examples/node) (the video's server) | — | Node | Fastify 5 + node-postgres, `node:cluster` workers | 587,000 | 310,000 | 125,000 | 2026-10-19 |
| 73 | [`go`](examples/go) (the video's server) | — | Go | Gin + pgx/v5 `pgxpool` | 248,000 | 230,000 | 124,000 | 2026-10-19 |
| 74 | [`rust`](examples/rust) (the video's server) | — | Rust | axum + SQLx | 413,000 | 227,000 | 114,000 | 2026-10-19 |
| 75 | [`crystal-kemal-faustinoaq`](submissions/crystal-kemal-faustinoaq) | [#19](https://github.com/arjaythedev/million-users-challenge/pull/19) | Crystal | Kemal 1.15, a process per core with `SO_REUSEPORT`, crystal-pg | 194,000 | 180,000 | 103,000 | 2026-10-30 |
| 76 | [`php-laravel-octane-gabidj`](https://github.com/arjaythedev/million-users-challenge/pull/38)‖ | [#38](https://github.com/arjaythedev/million-users-challenge/pull/38) | PHP | Laravel Octane on Swoole 6.2.4 (PHP 8.3), a server per port with a worker per core, coroutine PDO pool with prepared statements | 360,000 | 160,000 | 73,000 | 2026-11-01 |
| 77 | [`scala-http4s-sideeffffect`](submissions/scala-http4s-sideeffffect) | [#7](https://github.com/arjaythedev/million-users-challenge/pull/7) | Scala | http4s Ember 0.23 on cats-effect 3 (JDK 27), Skunk session pools with prepared statements | 192,000 | 112,000 | 64,000 | 2026-11-01 |
| 78 | [`python`](examples/python) (the video's server) | — | Python | FastAPI on Gunicorn/Uvicorn workers + asyncpg | 213,000 | 104,000 | 51,000 | 2026-10-19 |
| 79 | [`tex-cgi-Emsii3`](https://github.com/arjaythedev/million-users-challenge/pull/36)¶ | [#36](https://github.com/arjaythedev/million-users-challenge/pull/36) | TeX | C epoll front end on both ports feeding long-lived pdfTeX workers over FIFOs (JWT check and JSON in TeX), libpq async pool with prepared statements | 60,000 | 32,000 | 14,500 | 2026-11-01 |
| 80 | [`cmake-lots_of_things-IdkWhatToWriteHereOhManItWasAlreadyUsed`](https://github.com/arjaythedev/million-users-challenge/pull/17)ᵖ | [#17](https://github.com/arjaythedev/million-users-challenge/pull/17) | CMake | libevent listener + NASM launcher running `cmake -P` per request (HTTP, JWT, JSON in CMake script), libpq pool daemon over a unix socket | 1,000 | 1,000 | 1,000 | 2026-10-30 |
| 81 | [`css-chromium-0x95`](https://github.com/arjaythedev/million-users-challenge/pull/79)‡‡‡‡ | [#79](https://github.com/arjaythedev/million-users-challenge/pull/79) | CSS | Rust tokio front relaying requests to headless Chromium pages, where a generated 8 MB stylesheet does HTTP framing, routing, HS256 and JSON validation; Postgres wire protocol from the pages over a local WebSocket relay, prepared statements | 5,000 | 2,500 | 500 | 2026-11-05 |
| 82 | [`typescript-typelevel-Clickin`](submissions/typescript-typelevel-Clickin) | [#102](https://github.com/arjaythedev/million-users-challenge/pull/102) | TypeScript | App logic (HTTP parsing, routing, HS256 JWT, JSON) computed by the TypeScript 7.1 beta type checker once per request via `typescript/sync`, a Node 22 `cluster` worker per core with a hand-written Postgres wire client | 3,000 | 1,500 | 500 | 2026-11-08 |

The six example servers were scored for the video (2026-10-18/19) with borrowed load generators and the old doubling
search; the test, the boxes and the pass rules are otherwise the same.

\* Held, not merged yet: CI hasn't run on #4's scored commit yet. Scored at 51f1656.

† Held, not merged yet: #16 ships its libdeflate and picohttpparser sources as tarballs in `vendor/`, still to be
checked against upstream, and CI hasn't run on the scored commit yet. Scored at 4e6aad4.

‡ Held, not merged yet: #23's review points are fixed, but CI hasn't run on the scored commit yet. Scored at 6a5de6a.

§ Held, not merged yet: #25's toolchain is pinned (OTP 29.1.2 and Elixir 1.20.4, both SHA-256 checked), but CI
hasn't run on the scored commit yet. Scored at 7bd0237.

ᵖ Held, not merged yet: #17's `install.sh` was patched for the run only (it chowns `/app` to root, so `build.sh` as
`challenge` couldn't write there; the run used `chown -R challenge:challenge /app`), and it installs into `/app` and
`/tmp` instead of its own folder. Scored at 79c8ed4.

¶ Held, not merged yet: #36 keeps its per-request work files and FIFOs in `/dev/shm` (or `$TMPDIR`, else `/tmp`)
instead of its own folder. Scored at 8b8dc81.

\*\* Held, not merged yet: CI hasn't run on #41's scored commit yet (the `/proc/meminfo` read of 8dc228e is gone).
Scored at 0094729.

‖ Held, not merged yet: #38 accepts a JWT with no `exp`, which the SPEC requires. Scored at 525d311.

†† Held, not merged yet: CI hasn't run on #37's scored commit yet (the 560 Postgres connections of 5cf3dff are
capped at 512 now). Scored at 696ae01.

‡‡ Held, not merged yet: CI hasn't run on #42's scored commit yet. Scored at f633e9b.

§§ Held, not merged yet: CI hasn't run on #43's scored commit yet (the `/proc/stat` gzip governor of e392617 is
gone). Scored at ac1b494.

¶¶ Held, not merged yet: #44 accepts a JWT with no `exp`, which the SPEC requires. Scored at e04078d.

‖‖ Held, not merged yet: CI hasn't run on #47's scored commit yet (the box-wide `/proc/stat` reads of b6c8540 are
gone). Scored at 904bc7d.

††† Held, not merged yet: CI hasn't run on #65's scored commit yet (the early write replies and the box-wide
`/proc/meminfo` read of 6cf7986 are gone). Scored at 9866067.

‡‡‡ Held, not merged yet: CI hasn't run on #67's scored commit yet. Scored at ea20c5a.

§§§ Held, not merged yet: CI hasn't run on #69's scored commit yet. Scored at 99c3e1f.

¶¶¶ Held, not merged yet: CI hasn't run on #70's scored commit yet. Scored at bb2a4b0.

‖‖‖ Held, not merged yet: CI hasn't run on #74's scored commit yet. Scored at 0ece5ba.

\*\*\* Held, not merged yet: #75 accepts a JWT with no `exp`, which the SPEC requires, its `start.sh` reads the
box-wide `/proc/meminfo`, and CI hasn't run on the scored commit yet. Scored at 40a1451.

†††† Held, not merged yet: CI hasn't run on #76's scored commit yet. Scored at 9a8d8c8.

‡‡‡‡ Held, not merged yet: CI hasn't run on #79's scored commit yet. Scored at 819ea5d.

§§§§ Held, not merged yet: #81's `start.sh` reads the box-wide `/proc/meminfo` to set `GOMEMLIMIT` (outside its
folder), and CI hasn't run on the scored commit yet. Scored at 340df6d.

¶¶¶¶ Held, not merged yet: CI hasn't run on #82's scored commit yet. Scored at ecc5d38.

‖‖‖‖ Held, not merged yet: CI hasn't run on #83's scored commit yet. Scored at 8bf0680.

\*\*\*\* Held, not merged yet: CI hasn't run on #85's scored commit yet. Scored at 2b1cee5.

††††† Held, not merged yet: CI hasn't run on #27's scored commit yet (the early write replies of 2be67ff are gone).
Scored at 64f3d36.

‡‡‡‡‡ Held, not merged yet: CI hasn't run on #60's scored commit yet (the early write replies of 2ef44a7 are gone:
writes are now answered at the `ReadyForQuery` of their Sync). Scored at ab91f70.

§§§§§ Held, not merged yet: CI hasn't run on #61's scored commit yet (the CPU pinning and box-wide `/proc` reads of
74a3ba1 are gone). Scored at 523ba05.

¶¶¶¶¶ Held, not merged yet: CI hasn't run on #66's scored commit yet. Scored at 77b03cd.

‖‖‖‖‖ Held, not merged yet: CI hasn't run on #71's scored commit yet. Scored at ba806ca.

\*\*\*\*\* Held, not merged yet: CI hasn't run on #88's scored commit yet (the box-wide
`/proc/sys/net/ipv4/tcp_mem` read of 18351f2 is gone). Scored at 9686d0a.
ᵖᵖ Held, not merged yet: CI hasn't run on #64's head yet. Scored at f326862 with `CMakePresets.json` patched for the
run (`"version": 5` → `6`); the author's b56ef78 makes exactly that change, so the scored code is b56ef78.

†††††† Held, not merged yet: CI hasn't run on #86's scored commit yet (the extra `SELECT 1` / post check queries and
the missing-`exp` acceptance of 43fa200 are gone). Scored at 79bdcba.

‡‡‡‡‡‡ Held, not merged yet: CI hasn't run on #99's scored commit yet (the early write replies of c0bae62 are gone:
writes are now answered at the `ReadyForQuery` of their Sync). Scored at 776fa1f.

§§§§§§ Held, not merged yet: CI hasn't run on #94's scored commit yet. Scored at 748c9fa.

¶¶¶¶¶¶ Held, not merged yet: CI hasn't run on #106's scored commit yet. Scored at 7f6cba3.

‖‖‖‖‖‖ Held, not merged yet: CI hasn't run on #112's scored commit yet. Scored at 6f0a4a8.

\*\*\*\*\*\* Held, not merged yet: CI hasn't run on #113's scored commit yet, its `build.sh` walks every
`/proc/<pid>/exe` on the box (outside its folder), and its `start.sh` keeps an opt-in `PIN=1` `taskset` option
(rule 13). Scored at 1f03fde.

††††††† Held, not merged yet: #116's head moved to d57221d (a newer AmmarServer pin) during the run; that commit
is queued for its own run. Scored at 8cf55d9.
