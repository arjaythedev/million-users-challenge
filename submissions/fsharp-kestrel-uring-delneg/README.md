# fsharp-kestrel-uring-delneg: F# on its own io_uring threads, one per core, with its own Postgres client

| | |
|---|---|
| Language | F# on .NET 11 RC 3 (SDK `11.0.100-rc.3.26522.148`, pinned and SHA-512 checked in `install.sh`), ReadyToRun, tiered PGO, workstation non-concurrent GC |
| HTTP | Own HTTP/1.1 on raw io_uring (no liburing): one ring per I/O thread, multishot accept into fixed files, multishot recv with provided buffers |
| Host | Kestrel's host with a custom transport: the io_uring threads own the sockets, and requests never go through Kestrel's pipeline |
| Postgres driver | Own v3 wire-protocol client (extended protocol, binary rows, SCRAM-SHA-256), no pool library |
| gzip | ISA-L 2.31.1 igzip, level 0 (libdeflate 1.26 level 1 as an option), both built from pinned, SHA-256-checked source tarballs by `build.sh` |

```bash
sudo bash install.sh
bash build.sh
DATABASE_URL=postgres://feed:pw@db:5432/feed JWT_SECRET=million-users-challenge HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

## Design

**One I/O thread per core, nothing shared.** `Environment.ProcessorCount` threads (it follows the affinity mask, so
16, 8 and 4 cores size themselves). Each thread has its own io_uring ring, its own `SO_REUSEPORT` listening socket on
each of the two ports (the kernel spreads new connections over the threads), and its own Postgres connections.
No locks, no cross-thread hand-offs on the request path.

**An idle connection owns no buffer.** Accepted sockets go straight into the ring's fixed-file table (multishot
accept with `IORING_FILE_INDEX_ALLOC`), so they never get a process fd. Each socket has one multishot recv that
takes a 2 KB buffer from the thread's provided-buffer group only when bytes arrive; the request is parsed in place
and the buffer goes straight back. Only a request split over several receives is copied into a buffer of its own.
Per-connection state is a few entries in arrays indexed by the fixed-file slot (generation, state bits, length),
not an object per connection.

**Kernel 6.8.0-147 and buffer rings.** Ubuntu's 6.8.0-1xx kernels (-132, -135..-142, -145..-149) refuse
`IORING_REGISTER_PBUF_RING` with EINVAL: their `io_register_pbuf_ring` has the reserved-field check inverted
(`if (!memchr_inv(reg.resv, 0, ...)) return -EINVAL`, where upstream rejects non-zero fields). The server registers
normally first; on EINVAL it retries once with `resv[0] = 1`, which those kernels accept (#23 does the same);
if that fails too it falls back to `IORING_OP_PROVIDE_BUFFERS`. `LEGACY_BUFS=1` forces the fallback (tested).

**Postgres: few connections, pipelined, reads apart from writes.** Each thread opens its share of 64 read and 32
write connections (16 cores: 4 + 2 per thread; 4 cores: 16 + 8), in the thread's own ring. A query goes to the
first open connection of its kind with fewer than 8 queries in flight (the least loaded one when all are that
deep), so the queries of one wake-up fill a few connections instead of one query on each of many: fewer sends,
fewer Postgres backends woken. All queries a thread queues in one pass of its loop leave in one send per
connection. Against spreading them by fewest-in-flight, this cut instructions per request by 17%, cycles by 23%
and Postgres CPU per request by a third (`PG_DEPTH=0` restores that).
Every statement is prepared once per connection; parameters and results are
binary, and rows are read in place from the receive buffer. A connection that drops is reopened in the background
(50 ms, doubling to 1 s) while the others carry the load; with none open, `/health` answers 503.

**The SQL is SPEC.md's, one statement per request**, each in its own `Sync` (autocommit): the post `SELECT` with
`LIMIT 20` for the feed or `WHERE p.id = $1`, `INSERT ... RETURNING id, created_at`, `INSERT ... ON CONFLICT DO
NOTHING` (SQLSTATE 23503 -> 404). Nothing is cached across requests: every request reads Postgres while it is
served, and every token's HS256 signature, `alg` and `exp` are checked on every request.

**Responses by hand**: compact JSON in the spec's key order, `created_at` from the binary `timestamptz` formatted in
UTC and truncated to milliseconds, strings escaped (`"`, `\`, control characters). Bodies of 512 bytes or more are
gzipped when the client accepts it, with ISA-L's igzip at level 0 (#9 and #12 use it): `/feed` goes from 5,102 to
2,243 bytes, still two TCP segments, for ~40% less user CPU per request than libdeflate level 1 (1,901 bytes;
`GZIP_IMPL=libdeflate`).

**One wake-up for many events.** After a loop pass that did work, the thread waits in `io_uring_enter` for 32
completions or 1 ms, whichever comes first (`IORING_ENTER_EXT_ARG`; with `DEFER_TASKRUN` it isn't woken before),
so the requests and Postgres answers that arrive meanwhile share one pass and their queries one send per
connection. An idle thread waits for one completion. The idea is #10's and #11's 2 ms sleep after a busy
`epoll_wait`, and #30's 128 / 2 ms with io_uring. At ~8,500 requests/s on one core it cut loop passes from 1.96
to 0.13 per request, instructions per request by 9% and cycles by 15%, for ~2 ms more p95 (`WAIT_N=1` turns it
off).

**HTTP/1.1** keep-alive with no idle timeout, `TCP_NODELAY`, listen backlog 65535, pipelined requests answered in
order, `Expect: 100-continue`, `Connection: close`, HTTP/1.0 keep-alive.

## Settings

All optional; the defaults are what gets benchmarked.

| Variable | Default | |
|---|---|---|
| `THREADS` | `Environment.ProcessorCount` | I/O threads |
| `DB_READ_CONNS`, `DB_WRITE_CONNS` | 64, 32 | Postgres connections in total, split over the threads (at most 512) |
| `GZIP_IMPL` | `igzip` | `igzip` (ISA-L) or `libdeflate` |
| `GZIP_LEVEL`, `GZIP_MIN` | 0 (igzip), 1 (libdeflate); 512 | compression level; smallest body that is gzipped |
| `GZIP_TRAIN` | 0 | `1`: igzip level 0 rebuilds its Huffman table from a response body at most once a second (feed 1,961 instead of 2,243 bytes, same CPU) |
| `PG_DEPTH` | 8 | queries in flight per Postgres connection before the next one is used; `0`: fewest in flight |
| `WAIT_N`, `WAIT_US` | 32, 1000 | after a busy pass, wait for this many completions or this long |
| `NBUFS` | 8192 | 2 KB receive buffers per thread |
| `SLOTS` | ~4M / threads (power of two, at most 2^20) | fixed-file table per thread |
| `LEGACY_BUFS` | off | `1`: provided buffers through `IORING_OP_PROVIDE_BUFFERS` |
| `MUC_STATS` | off | `1`: per-thread counters every 5 s |

## Correctness

`test/run.sh`: 42/42 (buffer ring and legacy buffers).

## Local results (NOT official)

Measured on a Synteq VPS like the grader's but smaller (8 shared vCPUs, Xeon Platinum 8260, 16 GB, Ubuntu 24.04,
kernel 6.8.0-147, the grader's sysctls), with Postgres 18, `mload` and an idle-connection holder on the same box.
**These are not official numbers.** The server gets one vCPU (so one I/O thread); the load is 20,000 `mload`
users at `-think-scale 0.25` (~8,450 requests/s) plus 100,000 idle keep-alive connections. The deciding number is
CPU per request on the server's vCPU, counted with `perf stat` (instructions and cycles, user + kernel). On loopback
that includes the kernel's receive work for the clients' and Postgres's sockets too, which the grader's remote
machines do themselves.

Instructions and cycles per request on the server's vCPU (`perf stat`, 90 s, medians of 2-3 runs):

| build | instructions | cycles | p95 / p99 |
|---|---|---|---|
| #1 rust-tokio-cknutson12, same box and load | 167.7k | 172k | 1.5 / 4 ms |
| v1 (before the box's docker NAT rules were removed: its kernel share is ~7k instructions higher) | 148.7k | 152k | 1.8 / 4 ms |
| + igzip level 0 (e2) | 100.1k | 118k | 1.5 / 4 ms |
| + wait 32 / 1 ms (e4) | 90.8k | 101k | 3.5 / 6 ms |
| + fill Postgres connections to 8 deep (e6) | 75.5k | 82k | 6 / 30 ms |

Near the one vCPU's limit (36,000 users, ~15,200 requests/s), #1 needed 154k cycles per request at 90% busy and
this build 70-75k at 48-50%.

Memory, 4 threads: 67 MB RSS idle, 138 MB at 250,000 connections, 148 MB at 1,000,000 (held on this 16 GB box with
the client end of every connection on it too; all 1,000,000 answered). The kernel's share is ~4.2 KB per socket. Every experiment, kept or not, is in `experiments/worklog.md` and `experiments/autoresearch.jsonl`, with
the scripts in `experiments/scripts/`.
