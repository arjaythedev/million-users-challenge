# bend-epoll: a Bend 2 request kernel under a C epoll host

The Bend code in `src/` decides each request: which statement it runs, and which reply its
result gets. The C host in `host/` runs the sockets, the HTTP, the HS256 check and the
PostgreSQL wire protocol. Each round, the host hands the Bend code the events it saw, and the
Bend code answers with commands for the host to run. The Bend code has proofs, in `proofs/`.

- Language: Bend 2.0.40 for `src/` and the four `.bend` files in `host/`, which `bend` compiles
  to C. C11 for the rest of `host/`. clang builds both with `-std=c11 -O3`, and with a profile
  that the build takes itself (the section "The build's profile"). `install.sh` takes the
  first that apt offers of `clang-20`, `clang-19` and `clang`. On Ubuntu 24.04 that is
  `clang-20`, version 20.1.2 when this folder was checked in a clean container. Of the apt
  packages, 19 and 20 have the `preserve_none` calling convention that bend's C asks for.
- Framework and IO: none. `epoll`, one single-threaded process per CPU, HTTP/1.1 written by hand
  (`host/epoll.c`, `host/http.h`).
- Driver: none. `host/pgwire.c` speaks PostgreSQL protocol 3.0 itself: SCRAM-SHA-256, one Parse
  per statement at connect, then Bind and Execute per request with binary parameters and
  results, and no Describe.
- Compression: gzip for a body of 1 KB or more when the request accepts it. `start.sh` starts
  the server with `--gzip isal`: ISA-L at level 0 (`host/isal.h`), the call that the
  `c-epoll-unorig-username` submission makes. Two other encoders are in the binary and off:
  the host's own fixed-Huffman encoder (`host/gzip.h`, a C port of `Gzip.cs` from the
  `csharp-epoll-xeonfx` submission) and libdeflate at level 1. The Design list says why the
  stream is ISA-L's.
- Front: direct. Every process listens on `PORT` and `EXTRA_PORT`, on every address. `HOST` is
  not read.
- PostgreSQL connections: 16 per process, and at most one more per process, which cancels the
  statement on a connection the process gave up. On 16 CPUs that is 16 x (16 + 1) = 272 at
  most. A process refuses to start when (pool + 1) x processes is over 512. With a pool of 16
  that is above 30 processes.
- Processes: one per CPU that `nproc` reports, so a CPU affinity mask sets the count. They
  share the two ports through `SO_REUSEPORT` and share nothing else.
- Libraries: OpenSSL's libcrypto (SHA-256, for HS256 and for SCRAM), libdeflate and ISA-L, from
  Ubuntu 24.04's `libssl-dev`, `libdeflate-dev` and `libisal-dev`.

## Design

- **Rounds.** A process runs one loop (`round` and `serve` in `host/main.bend`). Each round the
  host waits in `epoll_pwait2`, reads its sockets and hands the Bend code a list of events: a
  request arrived, a batch went out, a batch was answered, a batch died, a connection closed.
  The Bend code returns its next state and a list of commands: queue a statement in the open
  batch, open the next batch, send a reply. The host runs the commands at the start of the
  next round. Bend accepts only a loop that ends, so the loop counts down 4,294,967,295
  rounds. Then the process closes its sockets and exits.
- **Reads.** A round's reads join one batch of at most 64 statements. The host writes a batch
  as one buffer, Bind and Execute per statement and one Sync, to the read connection with the
  fewest batches in flight. 12 of a process's 16 connections carry reads.
- **Writes.** Writes join their own batch on the other 4 connections, so a read never waits
  behind a commit. A write batch waits up to 5 ms for more writes and goes out under one Sync:
  one transaction, one commit. The reply to each write leaves only after the ReadyForQuery of
  that Sync has arrived with status idle, which PostgreSQL sends after the commit. When the
  database refuses one write (a like on a missing post, SQLSTATE class 23), it rolls the batch
  back. That write gets its 404, and the others go again without it.
- **The window.** After a wake that brought events and did not fill the event array, the next
  round sends the read batch and sleeps until 2 ms after that wake, so one wake serves what
  arrived meanwhile. The idea is the batching window of the `c-epoll-unorig-username`
  submission.
- **Under a backlog, the newest request first.** A round reads requests into a ring and takes
  up at most 64 of them. `start.sh` starts the server with `--ring lifo`, which changes three
  things in that, all at once. First, a round reads every connection that is ready, up to
  1,024, where it otherwise reads 64. Second, and because of the first, almost no wake fills
  the event array, so the window above follows every wake. Third, when the ring's oldest
  request has waited 10 ms, the round takes up the newest request first, and every eighth
  from the old end. The reason is the client's delayed acknowledgement. A reply that leaves
  within 40 ms carries the acknowledgement of its request, and a later one costs the server
  a packet more. Oldest first makes every request of a backlog wait the whole backlog, so
  once a process is behind, every reply is late, every request costs that packet, and the
  process stays behind. Newest first answers in time the requests that still can be. What
  it costs: a request that is already late waits longer, because the old end gets an eighth
  of what the rounds take up. A request with d older ones before it is among the first
  8 x (d + 1) that such rounds take up, at the latest, so behind a backlog it waits up to
  eight times what it would in order. And if a backlog does not clear, one reply in eight
  carries the backlog's whole age, where the challenge allows 1 % of the replies at 1 s or
  later: the flag wins where a backlog clears, and only there. Measured on a lab copy of the
  box on 2026-11-10, one run each. At 1,750,000 users the 95th percentile was 44.87 ms with
  the flag and 266.37 ms without, 0.096 % of the replies took 500 ms or more against
  1.684 %, and the server sent 1.942 packets a request against 2.367. The first two changes
  without the third (the wider read and the window, oldest first as before) gave 245.99 ms
  and 2.557 packets at that size: it is the order that keeps the server out of the late
  state. At 1,800,000 users the flag's run passed with a 95th percentile of 68.15 ms and
  0.166 % of the replies at 1 s or later; in its one bad quarter minute a ring's oldest
  request had waited up to 3.6 s. That is the cost, a sixth of what the rule allows. At
  1,700,000 users, which the server holds either way, 0.010 % of the replies took between
  480 ms and 1 s with the flag and none without. In a test of one process with 5,000
  requests at once (three runs a setting, five spikes a run), of the requests that arrived
  during the backlog 46.5 to 50.1 % waited 40 ms or more without the flag, 41.0 to 46.7 %
  with the first two changes alone, and 0.0 to 5.9 % with the flag; the oldest request's
  reply left between 36 ms earlier and 22 ms later than without. The binary's own default is `fifo`,
  and `BM_RING=fifo ./start.sh` gives it.
- **The gzip stream is ISA-L's.** The host's fixed-Huffman encoder is the better one for this
  server alone: 900 bytes against 972 for the same feed, and less CPU to write. The folder
  does not use it because of how the challenge's load tool reads a reply. `mload` inflates
  every reply with Go's inflater, which allocated 8,369 bytes per feed for the fixed-Huffman
  stream and 25,269 for ISA-L's (a Go benchmark of `mload`'s own `gunzip`). With a third of
  the allocation, a generator of 840,000 users collected every 38 seconds where it otherwise
  collects every 14 to 16, and each of those collections made its sending goroutines mark
  for 0.8 to 1.8 seconds. The requests it held back then arrived at 2 to 3 times the usual
  rate, and the server's slowest replies were the ones in those bursts. With ISA-L's stream
  the generator collects more often and for less time, and the bursts do not form. This is a
  fact about the two programs together, measured on a lab copy of the box at 1,400,000 users
  on 2026-11-10. `BM_GZIP=fixed ./start.sh` gives the other stream.
- **Database replies the host cannot frame.** A message of an unknown type, a length out of
  range, a message out of place, or a ReadyForQuery whose status is not idle fails that
  connection (`pg_frame` in `host/pg.c`, `pgw_scan` in `host/pgwire.c`). The requests of its
  batches get 500, and the connection is opened again. A batch with no answer for 5 s fails
  the same way.
- **HTTP.** A connection has 512 bytes of request buffer, and a larger buffer only while a
  request needs one. One request per connection is in flight. The host itself answers a head
  over 4,096 bytes (431), a body over 16,384 bytes (413), a transfer coding (411) and a bad
  `Content-Length` (400), and closes the connection. It never times out an idle connection. A
  process holds at most 1,048,576 descriptors.
- **Stops.** The host ends its process when a value from the Bend code does not match its own
  records (a row handle of another request, a command out of turn), and when an allocation
  fails. `start.sh` then stops the other processes and exits with that status (the last lines
  of `start.sh`).

## The build's profile

`build.sh` compiles the server twice. The first compile puts clang's counters into it
(`-fprofile-generate`). The trainer, `pgo/train.c`, runs that server for 8 seconds: two
processes, started through `start.sh`, against a PostgreSQL stand-in that the trainer holds
itself. It sends them the request mix of the challenge's load tool: the feed, a post, a like
in 15 turns of 100 and a new post in 2 of 100, each request with `Accept-Encoding: gzip`. The
second compile reads the counters (`-fprofile-use`) and writes the binary that `start.sh`
runs. A profile tells clang which branches and calls are hot. It changes how clang inlines
and lays out the same C. It changes no flag of the server and nothing the server does.

- **No profile is in this folder.** `build.sh` takes it on the machine that builds, from this
  source and with the clang that compiles it, so it fits both. The leaderboard's
  `cpp-iouring_eastl-hendy643` builds the same way: its `build.sh` trains at build time and
  falls back to a plain build.
- **What the training runs.** The stand-in speaks enough of PostgreSQL's protocol 3.0 for the
  server's own client (`host/pgwire.c`) to do its work: the five Parse messages at connect,
  Bind and Execute in pipelines, binary rows, one ReadyForQuery per Sync, and SQLSTATE 23503
  for a like on a post that does not exist. The header of `pgo/train.c` lists what it answers
  ("the stub"). It accepts every login, so the SCRAM code gets no counts. That code runs once
  per connection, at connect.
- **What the training touches.** The trainer opens sockets on 127.0.0.1 only, on ports the
  kernel picks. The build makes its files under `.local/build/`, with one exception: `clang`
  writes its temporary object files in `/tmp` and removes them. `mkdir -p` and clang's
  profile runtime (in a training server, when it writes its counters) also call `mkdir` on
  the directories above the one they want, up to the root. This folder and the directories
  above it exist, so those calls make nothing. The two server processes listen on every
  address for those seconds, as the server always does (`ep_listen` in `host/epoll.c`).
  Their database is the stand-in, and their `JWT_SECRET` is a constant of `pgo/train.c`.
- **What ends the training.** No process is left when `build.sh` exits, and the build looks
  for none: it reads no process table and matches no name. Every signal goes to a process
  that the sender started itself, or to the sender's own group.
  - The training runs as one process group. Its leader is a subshell of `build.sh`
    (`guard`), which bash starts in a new group. Under it run `timeout`, the trainer, the
    trainer's `start.sh` and the two server processes, and they stay in that group. One
    short-lived process does not: the port check of `start.sh` runs under a `timeout 2`,
    which makes a group of its own, and ends within 2 s.
  - The guard holds the read end of a pipe from `build.sh`. When that pipe closes, the guard
    sends SIGKILL to its own group, the one whose id is its own process id (`end_group`,
    which refuses any id but a whole number above 1). This is the only signal `build.sh`
    itself sends. `timeout`, which holds the limit, signals the one process it started, the
    trainer.
  - `build.sh` closes the pipe when the training has ended, well or not, when its limit has
    passed, and when SIGTERM, SIGINT or SIGHUP reaches `build.sh`. It then waits until no
    process holds the training's status pipe, for 10 s at most, and says so on stderr when
    one still does. Every process of the training inherits that pipe, the port check too,
    so none is alive when the next step starts or `build.sh` exits. A signal during a
    compile takes effect when that compile has ended, and `build.sh` exits 143.
  - Under the guard, the trainer stops its server on every way out. It signals one process,
    its own `start.sh`, and only while it has not collected that process's exit
    (`stop_child` and `signal_child` in `pgo/train.c`). `start.sh` names a worker to `kill`
    by its job number, and bash sends nothing for a job whose exit it has collected
    (`stop` in `start.sh`).
- **When SIGKILL ends `build.sh`**, the script can do nothing itself. During the training
  the kernel closes the guard's pipe and the guard kills the group. The repository's
  cleanup test (`bench/servers/bend-epoll/submission/cleanup-test.sh`, in an `ubuntu:24.04`
  container) fails when a process of the training is left 2 s after the SIGKILL. In two
  runs on 2026-11-11 none was left at its first look, 0.01 s and 0.02 s after. Two cases
  are not covered. Outside the training, the step that runs goes on to its end and then
  exits: a compile (`compile.sh` with `bend` or `clang`) was gone 2.2 s, 2.4 s and 6.8 s
  after the SIGKILL in three runs of the test. And when SIGKILL ends the guard, whether or
  not it also ends `build.sh`, nothing kills the group. The trainer still stops its server
  when it ends. If the trainer is killed, the kernel sends its `start.sh` a SIGTERM
  (`PR_SET_PDEATHSIG`), and `timeout` ends the trainer at the limit (`BM_PGO_LIMIT_S`, 60 s
  unless set, and 10 s later with SIGKILL). But a server process that does not act on
  SIGTERM, one that is stopped for example, then stays until something else ends it. The
  test shows that case with a SIGKILL to the guard alone and one server process stopped:
  `build.sh` waits 10 s for the training's pipe, says on stderr that a process still holds
  it, and exits 0 with that process left.
- **The profile can only help.** When `llvm-profdata` is not installed, when the training
  fails or takes over 60 seconds, or when clang refuses the profile, `build.sh` writes one
  line to stderr (`build.sh: built without the profile: ...`), builds without it and exits 0.
  `BM_PGO=off ./build.sh` builds without it on purpose.
- **What it is worth, and what limits it.** Measured on a desktop on 2026-11-10, on revision
  `6b1fa46` of the repository this folder comes from. At that revision a feed's rows cross to
  the Bend code one cell per row, and the gzip stream is the host's fixed-Huffman one. In
  interleaved windows of two processes at 30,000 requests a second (four passes), the profile
  this build takes cut the server's user-mode instructions per request by 5.0 % (36,210
  against 38,128) and its user-mode cycles per kernel-mode cycle by 6.9 %. Two windows of one
  binary differed by 0.05 % and 0.5 %. A profile taken the long way, under a fixed-rate load
  of the same mix against a real PostgreSQL, gave 5.1 % and 7.4 % in the same windows. That
  second profile was also run on a lab copy of the box at 1,400,000 users, one run with it
  and one without: the server's user time was 7.51 µs a request against 8.13, and its whole
  CPU time 46.1 µs against 47.1. The limit is the kernel: user mode is under a fifth of what
  a process spends on a request, and the rest is system calls and TCP, which a profile of the
  server's code does not change. A training without the stand-in (the server's canned
  answers) was worth 1.3 % of the instructions, so most of the gain is in the database
  client's path. Since those windows the folder starts the server with `--rows block` and
  `--gzip isal` (`start.sh`), and both change the path a profile lays out. With both flags
  on, a new profile was taken the long way and measured, on revision `b2d934c` and the same
  desktop, in 12 windows of 3 s over four passes: 5.0 % fewer user-mode instructions per
  request (31,825 against 33,494) and 5.6 % fewer user-mode cycles per kernel-mode cycle,
  where two windows of one binary differed by 0.05 % and 0.5 %. Three things limit what that
  says about this folder. It is a profile taken the long way, not the one this build
  takes in 8 seconds against the stand-in. And with `--gzip isal` the compression runs
  inside `libisal.so.2`, which this build does not compile and no profile reaches. And the
  folder now also starts the server with `--ring lifo`, which no window here ran. What this
  build's own profile covers is in its output: `build.sh: trained: ...` is the start line
  of a training process with its flags, and `build.sh: profile: ...` says how many
  functions have counts.
- **Not the same bytes twice.** The counters depend on how the kernel scheduled the training,
  so two builds of this folder differ. Each build prints what it trained on: the trainer's
  line (requests, seconds, the mix), the start line of one training process with its flags,
  how many functions the profile has counts for, and the clang command that used it.

## Rules

The first twelve rows are rules 2 to 13 of the challenge's README. The last five are the
topics the maintainer's leaderboard notes flag at the edge of the rules. The last column names
a file and the function, table or block to read in it.

| Rule | How this folder meets it | Where to look |
|---|---|---|
| 2. PostgreSQL at `DATABASE_URL` is the only store, with no server-wide setting | Every connection the server opens goes to the host in `DATABASE_URL`, with one session parameter, `client_connection_check_interval`, in its startup message. | `host/pg.c`, `pg_open_run` |
| 3. The queries of `SPEC.md`, one per request, and no work moved into PostgreSQL | The server prepares five statements, `SELECT 1` for `/health` and the four of `SPEC.md`, and builds the JSON itself. | `host/sql.h`, `pg_sql` |
| 4. No change to the schema or the data's shape | Those five statements are the only ones a connection parses, and none creates, alters or drops anything. | `host/pg.c`, `pg_prepare` |
| 5. At most 512 connections | A process refuses to start when (pool + 1) x processes is over 512. | `host/epoll.c`, `epoll_open_run` |
| 6. Pass the test suite | `test/run.sh` printed `passed=42 failed=0` for this folder in a clean `ubuntu:24.04` container that had run `install.sh`. | `build.sh`, `start.sh` |
| 7. No caching across requests | A row is a view into a database connection's receive buffer for one round, and the host releases every row at the start of the next round. | `host/pg.c`, `pg_release` |
| 8. Writes are committed before the response | The reply to a write waits for the ReadyForQuery of its batch's Sync, and the host accepts only status idle there. | `host/pg.c`, `pg_frame` |
| 9. HS256 and `exp` checked on every request, no hard-coded responses | For every request on a write route, the host computes the token's signature, compares it, and compares `exp` with the clock. | `host/http.h`, `http_auth` |
| 10. 16 vCPUs and 32 GB, and out of memory loses | An open connection costs 545 bytes of the host's tables, in pages that are mapped when first used. | `host/epoll.c`, `ep_table` |
| 11. No proxy, ports 80 and 81, a million keep-alive connections | Every process binds `PORT` and `EXTRA_PORT` itself, with `SO_REUSEPORT`. | `host/epoll.c`, `ep_listen` |
| 12. Built from source on the box, versions pinned, no binary shipped | `install.sh` checks the bend 2.0.40 archive against a pinned sha256, and takes clang, libcrypto, libdeflate and ISA-L at Ubuntu 24.04's package versions. | `install.sh`, the constants at its top |
| 13. Nothing tuned on the box, no pinning | No script of the folder sets a kernel parameter, a CPU set or a priority, and the server's code sets none. `start.sh` has no pinning option: every server process runs on the CPUs of the process that started `start.sh`. A server process raises one limit, its own soft limit of open files, to the hard limit it was started with. | `start.sh`, the worker loop; `host/epoll.c`, `ep_nofile` |
| Tokens without `exp`, extra queries | A token whose payload has no numeric `exp` later than the clock gets 401. | `host/http.h`, the end of `http_auth` |
| Reads of `/proc` and `/sys` | No file of this folder but this README names a path under `/proc` or `/sys`, and nothing in it lists processes. A server process opens five such files, once each: four for its CPU count and one in libcrypto. None holds a load or memory figure. The notes under this table name them. | the notes under this table |
| CPU pinning | The folder has no pinning option and calls nothing that sets a CPU set. `start.sh` starts `WORKERS` processes, by default `nproc`, the number of CPUs it may run on, and each keeps the CPU set of `start.sh` itself. | `start.sh`, the worker loop |
| Early write replies | In the Bend code, a reply that reports a write's success needs a Synced event that reports that write's commit. This is the law `success_after_commit`. | `proofs/LAWS.bend`, `success_after_commit` |
| A reply on CommandComplete, before the ReadyForQuery that commits | The host leaves a batch's replies unread in the receive buffer until its ReadyForQuery has arrived. | `host/pg.c`, `pg_results` |

Six things a reader of this table should know:

- The binary has a test mode, `--db canned`, that answers from files and opens no database.
  `start.sh` passes `--db pg` unless `DB` is set (its defaults block), and the mode needs
  golden files that this folder does not hold.
- Each server process reads its CPU count once at start. The code is the Bend runtime's
  (`cpu_count`, in the C that `bend` writes to `.local/build/bend-epoll.c`, not in `host/`).
  It asks the C library for the number of online CPUs, for which glibc opens
  `/sys/devices/system/cpu/online`. It reads the process's own CPU set
  (`sched_getaffinity`). It opens `/sys/fs/cgroup/cpu.max` and, when that file does not
  hold two numbers, the cgroup v1 files `cpu.cfs_quota_us` and `cpu.cfs_period_us` under
  `/sys/fs/cgroup/cpu`. The count is only the runtime's default number of threads, and
  `start.sh` sets that number itself (`BENDRTS`, `--threads 1`). Seen with `strace` in an
  `ubuntu:24.04` container: a server process opens all four files, and the two v1 files do
  not exist there.
- Ubuntu's libcrypto opens `/proc/sys/crypto/fips_enabled` once in a process, on its first
  call that needs a provider. In a server process that is the SHA-256 of the first database
  login (`SHA256` in `host/pgwire.c`), and in the trainer the first token's HMAC. Seen with
  `strace` against a PostgreSQL that asks for SCRAM. A server process opened no other path
  under `/proc` or `/sys` there or in the training.
- The tools the build runs open files under `/proc` and `/sys` for themselves when they
  start. `clang`, `llvm-profdata` and `grep` open entries of their own process under
  `/proc/self`. Ubuntu's `mkdir`, `mv` and `find` open `/proc/filesystems` and
  `/proc/mounts`. `bend` opens entries of its own process, the kernel's and its cgroup's
  settings for memory and CPUs, and one tracing file,
  `/sys/kernel/debug/tracing/trace_marker`, which was not there. None of them lists
  processes.
- Between requests a process keeps its connections, the five prepared statements, the HMAC key
  state for `JWT_SECRET` (`http_key` in `host/http.h`) and the SCRAM salted password of its own
  database login (`pgw_hi` in `host/pgwire.c`). It keeps no row, no response and no token
  verdict.
- For rule 12: the folder holds no build output and no profile. `build.sh` takes the profile
  where it builds, from this source (the section "The build's profile"). `install.sh` adds
  the two packages that needs, `llvm-N` and `libclang-rt-N-dev` for the clang it chose, at
  Ubuntu 24.04's versions.

## What is formally verified

The Bend code in `src/` has proofs in `proofs/`. The laws state what that code decides for a
request, a reply and a batch of database results. The proofs were checked with
`bend PROOF.bend` in the Bend Million repository at commit 921d480. They do not cover the
C host, the PostgreSQL client, the Linux kernel, the Bend compiler, clang, libc, the three
libraries the host calls (libcrypto, libdeflate, ISA-L) or the binary. ISA-L writes every
gzipped reply. Its stream was tested on Zen 3 CPUs only, and not on a CPU with AVX-512,
where the library runs other code.
The second checker, `bend PROOF.bend --verdict`, needs Lean, which `install.sh` does not
install. So the second checker was not run in this folder or on the box. It ran in the
repository at that commit, with the same result.

As this folder runs the server, the proofs do not cover the posts in a feed's reply.
`start.sh` starts the server with `--rows block`, and that flag takes those posts out of the
proofs' reach. The law is `feed_ok_passes_rows`: a feed query that found rows answers with
exactly those rows. It is unchanged and proved as written. What changes is what the Bend
code is given. With `--rows each` it gets one row per post: the post's id, its like count and
a handle for the rest. The law then covers how many posts the reply holds, their order,
their ids and their like counts, given that the host writes what it is passed, which no
proof covers. With `--rows block` it gets one cell for the whole answer: a count and the
answer's first handle. The law then covers only that one answer comes back for the request.

Still proved with `block`: that one answer, and what the laws say about everything else the
Bend code decides. That is the status of a reply, the routing, which request gets which
answer, and the write path.

Not proved with `block`: the number, the order, the ids and the like counts of the posts in
a feed's reply. For those four the C host is trusted. `pg_outcome` in `host/pg.c` holds a
statement's rows as neighbours, in the order PostgreSQL sent them. `pg_apart` in
`host/pg.c` stops the process (exit 70) when a second answer of one request would begin
right after its first. `pg_run` in `host/pg.c` stops it unless the cell's run is rows of
that request with no row of the request on either side, so the run is one statement's
whole answer. `ep_send_posts` in `host/epoll.c` renders that run in table order, with each
row's own id and like count. That the request has no other answer in the round rests on
two things the host does not check: the Bend code queues one statement per request that
arrives (`arrived.commands` in `src/policy.bend`), and request numbers do not repeat until
a process has taken 2^32 requests. The evidence is tests of C code, and none of it is a
proof: `tests/host/rows_test.mjs` and `tests/host/oracle_test.mjs` in the repository, and
the challenge's 42 checks, which pass with the flag.

One check went with the rows. With `each`, `pg_row` in `host/pg.c` compared the id the Bend
code passed with the id of the row behind the handle, so Bend code that swapped two handles
of one answer stopped the process. With `block` there is no id beside a handle, and that
check has no counterpart.

`BM_ROWS=each ./start.sh` puts a feed's posts back under the proofs. On a lab copy of the box
at 1,400,000 users, with ISA-L's stream in both runs and one run each, the server's user
time was 6.30 µs a request with `block` and 7.98 with `each`.

`--ring lifo` takes nothing out of the proofs' reach. It changes the order in which a
process takes up the requests of different connections. That order was the C host's before
the flag, and no law speaks of it: the laws hold for the events in whatever order the host
hands them over. A connection's own requests keep their order.

The proofs do not cover the four `.bend` files in `host/` either. Those files join the Bend
code in `src/` to the C host. To run the first checker in this folder after `install.sh`:

```bash
BEND_NO_TELEMETRY=1 bend proofs/PROOF.bend    # prints ALL PROOFS CHECK
```

## Files

| Path | What |
|---|---|
| `install.sh` | Runs once as root: apt packages, clang with `llvm-profdata` and the profile runtime, and bend 2.0.40 into `/opt/bend-2.0.40` |
| `build.sh` | Builds `.local/build/bend-epoll`: an instrumented compile, the training run, the merge, and the compile that uses the profile. Builds without the profile when a step fails, or with `BM_PGO=off` |
| `compile.sh` | One compile: `bend` writes `.local/build/bend-epoll.c`, and clang builds `.local/build/bend-epoll` from it and `host/pgwire.c`. `build.sh` calls it |
| `pgo/train.c` | The training run, a build tool: a PostgreSQL stand-in and a request driver on 127.0.0.1. Nothing of it is in the server |
| `start.sh` | Starts one process per CPU, in the foreground. Every default is in one block |
| `README.md` | This file |
| `.gitignore` | `.local/`, the build output |
| `host/` | The C host, and the four `.bend` files that join it to `src/` |
| `src/` | The Bend code that decides each request |
| `proofs/LAWS.bend` | The laws about `src/` |
| `proofs/PROOF.bend` | Their proofs |

`host/`, `src/` and `proofs/` are copies from the Bend Million repository. Their comments cite
files of that repository (`docs/`, `bench/`, `vendor/`) that this folder does not hold.

## Running

```bash
sudo ./install.sh   # apt packages and clang-20, then bend 2.0.40, sha256 checked
./build.sh          # downloads nothing, trains for 8 s on 127.0.0.1, writes .local/build/bend-epoll
DATABASE_URL=postgres://feed:<password>@<database host>:5432/feed JWT_SECRET=<secret> \
	HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 ./start.sh
```

`start.sh` needs only those variables. Its other settings (`WORKERS`, `POOL`, `SOCK_OPTS`,
`WINDOW_US`, `DB`, `BM_GZIP`, `BM_ROWS`, `BM_RING`, `BENDRTS`) each have a default in its
defaults block.
