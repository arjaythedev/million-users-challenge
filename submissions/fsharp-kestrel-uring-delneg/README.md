# fsharp-kestrel-uring-delneg: F# on its own io_uring threads, one per core, with its own Postgres client

| | |
|---|---|
| Language | F# on .NET 11 RC 3 (SDK `11.0.100-rc.3.26522.148`, pinned and SHA-512 checked in `install.sh`), ReadyToRun, tiered PGO, workstation non-concurrent GC |
| HTTP | Own HTTP/1.1 on raw io_uring (no liburing): one ring per I/O thread, multishot accept into fixed files, multishot recv with provided buffers |
| Host | Kestrel's host with a custom transport: the io_uring threads own the sockets, and requests never go through Kestrel's pipeline |
| Postgres driver | Own v3 wire-protocol client (extended protocol, binary rows, SCRAM-SHA-256), no pool library |
| gzip | libdeflate 1.26, level 1, built from its pinned, SHA-256-checked source tarball by `build.sh` |

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
open connection of its kind with the fewest queries in flight; all queries a thread queues in one pass of its loop
leave in one send per connection. Every statement is prepared once per connection; parameters and results are
binary, and rows are read in place from the receive buffer. A connection that drops is reopened in the background
(50 ms, doubling to 1 s) while the others carry the load; with none open, `/health` answers 503.

**The SQL is SPEC.md's, one statement per request**, each in its own `Sync` (autocommit): the post `SELECT` with
`LIMIT 20` for the feed or `WHERE p.id = $1`, `INSERT ... RETURNING id, created_at`, `INSERT ... ON CONFLICT DO
NOTHING` (SQLSTATE 23503 -> 404). Nothing is cached across requests: every request reads Postgres while it is
served, and every token's HS256 signature, `alg` and `exp` are checked on every request.

**Responses by hand**: compact JSON in the spec's key order, `created_at` from the binary `timestamptz` formatted in
UTC and truncated to milliseconds, strings escaped (`"`, `\`, control characters). Bodies of 512 bytes or more are
gzipped (libdeflate level 1) when the client accepts it: `/feed` goes from ~5.1 KB to ~1.9 KB.

**HTTP/1.1** keep-alive with no idle timeout, `TCP_NODELAY`, listen backlog 65535, pipelined requests answered in
order, `Expect: 100-continue`, `Connection: close`, HTTP/1.0 keep-alive.

## Settings

All optional; the defaults are what gets benchmarked.

| Variable | Default | |
|---|---|---|
| `THREADS` | `Environment.ProcessorCount` | I/O threads |
| `DB_READ_CONNS`, `DB_WRITE_CONNS` | 64, 32 | Postgres connections in total, split over the threads (at most 512) |
| `GZIP_LEVEL`, `GZIP_MIN` | 1, 512 | libdeflate level; smallest body that is gzipped |
| `NBUFS` | 8192 | 2 KB receive buffers per thread |
| `SLOTS` | ~4M / threads (power of two, at most 2^20) | fixed-file table per thread |
| `LEGACY_BUFS` | off | `1`: provided buffers through `IORING_OP_PROVIDE_BUFFERS` |
| `MUC_STATS` | off | `1`: per-thread counters every 5 s |

## Correctness

`test/run.sh`: 42/42 (buffer ring and legacy buffers).

## Local results (NOT official)

Coming: measured on a Synteq box (8 shared vCPUs, 16 GB), with the server on 4 of them. Every experiment, kept or
not, goes into `experiments/worklog.md` and `experiments/autoresearch.jsonl`.

## Credits

- Our multicore entry (F#, multicore-server-challenge #87) and twelve-dollar entry (F#): the io_uring layer, the
  HTTP parser and the Postgres client, which started from csharp-sockets-jh-d (multicore #22, MIT).
- rust-tokio-cknutson12 (#1): the shape (thread per core, 64 + 64 connections, reads apart from writes, gzip at
  level 1 from 512 bytes).
- cpp-io_uring1m-mattiasca06 (#23): the buffer-ring retry on Ubuntu's 6.8 kernels.

Built with Claude (Anthropic).
