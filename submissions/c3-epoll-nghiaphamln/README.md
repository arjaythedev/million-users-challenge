# c3-epoll: C3, one epoll loop per core, its own Postgres wire protocol and gzip

- Language: [C3](https://c3-lang.org) 0.8.5_1 (official static Linux tarball, SHA-256 pinned in `install.sh`),
  `-O3 --safe=no`, single module, `--x86cpu=sse4` on the box
- Dependencies: libc and ISA-L 2.31.1 (BSD-3-Clause), source SHA-256 pinned in `build.sh`, linked statically
  for its CPU-dispatched gzip CRC only. The Postgres driver (`pg.c3`), the HTTP parser (`http.c3`), the JSON
  parser (`json.c3`) and the gzip encoder (`gz.c3`) are part of the program; HMAC-SHA256, SHA-256, PBKDF2 and MD5
  come from the C3 standard library.
- Front: direct (no Nginx), ports `PORT` and `EXTRA_PORT`
- Postgres connections: 64 on the benchmark's 4-, 8- and 16-core configurations;
  in general, `threads * max(2, floor(64 / threads))`
- Compression: when the request accepts gzip, bodies from 1,024 to 65,535 bytes are compressed
  (normally `/feed` in the load test). The 5,102-byte golden feed becomes 2,428 bytes; other sizes are sent plain.

## Local wire-format validation (2026-11-07)

Three paired ARM runs used separate client/app/Postgres network namespaces with offloads disabled,
20,000 clients, think-scale 0.125, a 30 s ramp and a 300 s hold. Median PostgreSQL-to-app IPv4 bytes
(including TCP/IP headers) fell from 1,409.86 to 1,359.16 per request (3.60%). Both directions combined
fell from 1,456.16 to 1,412.89 bytes (2.97%), including the larger Bind format list. Throughput stayed
at about 16.86k req/s and all six runs had zero errors. These are traffic measurements, not a new capacity score.

The 8-CPU low-load check, repeated in reverse order, showed a local trade-off: median p99 went from
6.88 to 9.91 ms. It does not establish maximum 8-core capacity. A 300K-connection check held for
300 s with zero errors and p99 21.06 ms; app cgroup memory was about 1.11 GiB. Low-density RSS varied
with transparent huge pages; a process-local THP-disabled memory audit found matching allocated capacity
and approximately 79.5 MiB for either version. Production THP settings are unchanged.

API/protocol/gzip checks passed on ARM and emulated x86-64, including exact IDs above 2^53 and at
INT64_MAX, numeric counts and millisecond timestamps. Native x86 performance is not yet measured.
All benchmark tools and raw data remain outside the submission.

## Earlier CRC measurements (2026-11-04)

The ISA-L CRC change reduced steady application CPU/request from 29.14 to 27.73 µs (about 4.8%) at
200,000 connections. A separate 300,000-connection run completed 4,723,024 requests with zero errors,
p95 10.50 ms and p99 41.44 ms. These are ARM Docker measurements, not official leaderboard scores.
The test VM ran on an Apple M1 Pro with 10 vCPUs, 4 GiB RAM and 1 GiB swap: four CPUs for the app,
four for PostgreSQL and two for the load generator. Benchmark tooling and raw results are kept outside
this submission.

## Design

- **Threads**: one per CPU in the affinity mask (`sched_getaffinity`, read only), each with its own epoll set.
  Halving the loops (tried in an earlier revision) cut CPU/request locally, but the same change cost another
  entry 8-core capacity on the benchmark box (#90), so it was reverted.
- **Listeners**: every thread opens its own `SO_REUSEPORT` listener on **both** ports; the kernel spreads new
  connections over the threads. A connection lives on its thread for its whole life: no locks, no hand-offs.
- **Postgres** (`pg.c3`): a client for exactly these five statements. The extended protocol over a plain TCP
  socket: Parse once per connection, then Bind/Execute with binary parameters and per-column results, pipelined,
  with explicit Syncs. There is no row object: a `DataRow` is rendered to JSON straight from the socket buffer
  (`put_post_row`), without allocating row objects or copying fields into an intermediate result object.
  Feed/post IDs and like counts arrive as decimal text and are copied into JSON; timestamps remain binary.
  Other statements keep binary results. JSON is written into the response arena. Auth is SCRAM-SHA-256 (the box), with MD5 and password fallbacks.
  - Reads: each connection's reads queued in one loop iteration share one Sync. The selector prefers an
    existing batch while it has fewer than 64 statements, then falls back to the least-loaded connection.
    `PACK=64` is a batching preference, not a hard limit on batch size or outstanding queries.
  - Writes go to separate connections (a quarter of each thread's, at least one), each with its own Sync, so
    each is its own transaction: a like on a missing post fails alone (`23503` -> 404) and a slow commit never
    holds up reads. The response is held until that Sync's `ReadyForQuery`, which the server sends after the
    commit (rule 8).
  - A lost connection fails ordinary queued requests with 500 (`/health` gets 503). Reconnect is attempted
    from request dispatch, at most once per clock second per thread. Setup uses blocking sockets with 3 s
    send/receive timeouts; these are not an end-to-end deadline, and reconnect can stall the HTTP loop.
- **HTTP** (`http.c3`): requests are parsed in place from a per-thread 64 KB read buffer. While a request waits
  for Postgres its connection is "busy"; pipelined requests behind it wait in a per-connection buffer, so
  responses stay in order. A response is built in a per-thread arena and normally sent with one `write`,
  gzip included; short writes are buffered and resumed on `EPOLLOUT`. Idle keep-alive connections have no timeout.
- **gzip** (`gz.c3`): greedy LZ77 with a single-probe hash table (four bytes hashed, one lookup per position,
  no chains; matches are compared eight bytes at a time) and the fixed Huffman block, in one pass. The feed
  (5.1 KB) comes out at ~2.4 KB; a per-body dynamic tree would save ~15% of the bytes but no TCP segment (two
  either way) and costs twice the instructions, so it was dropped. The earlier scalar-CRC version measured
  ~90k instructions per feed (callgrind); CRC now uses ISA-L. Bodies under 1 KB are sent plain.
- **Batching window**: after a loop iteration that handled 1–31 events, the thread sleeps 2 ms before
  the next `epoll_wait`, so one wake-up serves everything that arrived meanwhile and the reads among it share
  one Sync. Iterations with 32 or more events skip this explicit pause. The decision counts epoll events,
  not completed requests. In earlier local tests, 5 and 10 ms saved no CPU at ~4.6k req/s on 4 threads,
  while p95 doubled and quadrupled.
- **Validation**: strict RFC 8259 JSON, HS256 JWT with signature and a required `exp` checked on every request
  (the HMAC key schedule is computed once and copied per request), body trimming as JavaScript's
  `String.trim`, ids as positive decimal integers.

## Memory per connection

- User space: one 40-byte slot per descriptor in a table sized by `RLIMIT_NOFILE` (up to 16M), allocated with
  `calloc` (pages are touched only when used, so 1M connections is 40 MB). Buffers exist only while a request
  or response is split across reads or writes.
- Per thread: a 256 KB response arena, a 256 KB gzip buffer, a 64 KB read buffer, and the Postgres receive
  buffers (256 KB each, grown on demand).

## Rules notes

- Queries: five prepared statements serve the feed, single-post read, post insertion, like insertion and
  `SELECT 1` health check, using the SQL in `SPEC.md`. One statement runs per request; no response data or
  JWT verifications are cached across requests.
- Writes: each write commits in its own transaction before its response is sent.
- Connections: 64 to Postgres on the benchmark's 4-, 8- and 16-core configurations, below the rule's 512 limit.
  The sizing formula above is not a global cap: more than 32 worker threads open more than 64 connections.
- The box (rule 13): nothing outside the process is touched. The affinity mask is read, not set; `setrlimit`
  raises the process's own soft `RLIMIT_NOFILE` to the hard limit; socket options are `SO_REUSEPORT`,
  `SO_REUSEADDR` and `TCP_NODELAY`. No sysctl, no CPU pinning, no priorities.
- `install.sh` installs the toolchain (c3c tarball, build-essential and nasm). `build.sh` downloads and
  verifies the ISA-L source before compiling the static archive; the server still implements its own LZ77
  and fixed-Huffman encoder.

## Earlier local measurements (before the CRC change)

Docker on an Apple-silicon Mac (Colima VM, 4 CPUs, 3 GB), Postgres 18 in a container with the box config
scaled down (`shared_buffers=384MB`); server, Postgres and the load generator share the same 4 CPUs, so the
numbers are rough.

- `test/run.sh`: `passed=42 failed=0` in a clean Ubuntu 24.04 container that ran `install.sh`.
- mload, 5,000 users over both ports, `-gzip`, 15 s ramp + 45 s hold, fresh database copy:

| think-scale | req/s | p95 | p99 | errors | server CPU per request | peak RSS |
|---|---|---|---|---|---|---|
| 0.5 | 969 | 5.0 ms | 6.4 ms | 0 | ~39 us | 3 MB |
| 0.1 | 4,653 | 4.5 ms | 5.3 ms | 0 | ~19 us | 3 MB |

- mload, 150,000 users over 4 loopback addresses x 2 ports (the VM temporarily at 8 GB for the load generator),
  `-gzip -think-scale 2.5`, 120 s ramp + 90 s hold, run three times: all 150,000 connections held every time,
  0 errors, pass; server RSS 9-10 MB (150,079 descriptors), ~13% of one CPU at ~6.5k req/s. The load
  generator's own latency figures varied a lot between the runs (hold p95 from 5 ms to 200 ms) while it was
  using 5 GB and the same 4 CPUs as the server; an independent prober on its own keep-alive connection
  (10 req/s, monotonic clock) saw the server answer within 50 ms in 42 of 44 five-second windows, 59 ms in one,
  and one 912 ms request during the ramp. Separating the server from the load generator needs a second
  machine, which is how the challenge itself measures.
- Syscalls per request (strace -c): 5.0 — read, write, epoll_wait, the batching nanosleep, and the Postgres
  read/write amortized over a batch. callgrind puts the server's own code at ~85k instructions per request
  outside gzip. The ~180k gzip instructions/feed figure predates the fixed-Huffman change;
  earlier profiling recorded a reduction from 182k to 89k after that change. Neither is an instruction count
  for the ISA-L CRC build.
- A consistency check (pipelined requests on one connection, a body split across two writes with
  `Expect: 100-continue`, garbage and truncated requests, 300 connections dropped mid-request): every response in
  order and correct, server alive, no core dump.

## Running

Run on Linux. From the repository root:

```bash
cd submissions/c3-epoll-nghiaphamln
sudo ./install.sh   # c3c 0.8.5_1 into /opt/c3, build-essential, nasm
./build.sh          # build pinned ISA-L, then link the C3 server -> bin/server
DATABASE_URL=postgres://user:pass@host:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 ./start.sh
```

`install.sh` downloads the Linux x86-64 compiler used on the Ubuntu 24.04 benchmark box. Native ARM tests
used a separately installed ARM C3 0.8.5_1 compiler; `build.sh` accepts its path through `C3C`.

## Validation

From the repository root, run:

```bash
bash test/run.sh submissions/c3-epoll-nghiaphamln
```

This uses the repository's shared test suite. It requires the `feed_seed` database; configure the connection
with libpq environment variables (`PGHOST`, `PGPORT`, `PGUSER`, `PGPASSWORD`) as needed. The runner recreates
`feed_test` from that seed.

The updated server passed 42/42 API checks on native ARM Linux and on a clean Ubuntu 24.04 x86-64
container under emulation, including independent gzip checks on both ports. Local gzip regression checks
also passed 19 inputs on each architecture. The installer prints the complete compiler version to avoid
SIGPIPE under `pipefail`.
