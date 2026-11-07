# fsharp-uring-delneg: F# on raw io_uring, no framework, its own HTTP and Postgres code

| | |
|---|---|
| Language | F# on .NET 11 RC 3 (SDK `11.0.100-rc.3.26522.148`, pinned and SHA-512 checked in `install.sh`), JIT with tiered PGO, ReadyToRun, workstation non-concurrent GC. No AOT |
| HTTP | Own HTTP/1.1 on raw io_uring (no liburing): one ring per I/O thread, multishot accept into fixed files, multishot recv with provided buffers |
| Framework | None: a console app (`Microsoft.NET.Sdk`) whose `main` starts the I/O threads |
| Postgres driver | Own v3 wire-protocol client (extended protocol, binary rows, SCRAM-SHA-256), no pool library |
| gzip | ISA-L 2.31.1 igzip level 0 (libdeflate 1.26 or a pure F# encoder as options), built from pinned, SHA-256-checked source tarballs by `build.sh` |

```bash
sudo bash install.sh
bash build.sh
DATABASE_URL=postgres://feed:pw@db:5432/feed JWT_SECRET=million-users-challenge HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

This is our merged #35 (`fsharp-kestrel-uring-delneg`) with Kestrel's host taken out, plus the fixes that came
out of studying why its 5-minute holds failed where its 2-minute runs passed.

## Design

**One I/O thread per core, nothing shared.** `Environment.ProcessorCount` threads (it follows the affinity mask the
box gives the process, so 16, 8 and 4 cores size themselves). Each thread has its own io_uring ring
(`SINGLE_ISSUER | DEFER_TASKRUN | COOP_TASKRUN`), its own `SO_REUSEPORT` listening socket on each port and its own
Postgres connections. No locks and no hand-offs between threads on the request path.

**An idle connection owns no buffer.** Accepted sockets go straight into the ring's fixed-file table (multishot
accept with `IORING_FILE_INDEX_ALLOC`), so they never get a process fd. Each socket has one multishot recv that
takes a 2 KB buffer from the thread's provided-buffer ring (16,384 of them, 32 MB) only when bytes arrive. The request is parsed in place
and the buffer goes straight back; only a request split over several receives is copied. Per-connection state is
a few entries in arrays indexed by the fixed-file slot.

**Receives that find no buffer are all re-armed.** When a burst empties the buffer ring (after a long pause, say),
the multishot receives that got `ENOBUFS` end and must be armed again. #35 re-armed at most 32 of them per loop
pass. Once every buffer is back in the ring at the end of a pass, this entry re-arms all of them. On our rig, with
buffers made scarce on purpose (`NBUFS=64`), the old budget turned a Postgres stall into a p95 of 2.1 s at 1M
connections where re-arming all gave 0.3 s.

**Ubuntu 6.8 kernels and buffer rings.** Ubuntu's 6.8.0 kernels -132, -135..-142 and -145..-149 refuse
`IORING_REGISTER_PBUF_RING` with `EINVAL` (the reserved-field check is inverted, Launchpad #2162843). The server
registers normally first; on `EINVAL` it retries once with `resv[0] = 1`, which those kernels accept; if that fails
too it falls back to `IORING_OP_PROVIDE_BUFFERS`. `LEGACY_BUFS=1` forces the fallback (42/42 that way too).

**Postgres: queries spread over more connections, at most 32 in flight on each.** Each thread opens its share of
192 read and 64 write connections (4 cores: 48 + 16 per thread; 256 in all, under the 512 limit). A query goes to
the first connection of its kind with fewer than 32 queries in flight, so the queries of one wake-up leave in a few
sends and wake few backends. #35 filled each connection to 128: one lost segment on a Postgres connection could
then hold up to 128 queries, and everything queued behind them, for a retransmission timeout. 32 caps that at a
quarter while a busy connection still carries flights long enough (32 feed answers are ~30 segments) for a loss in
the middle to be repaired by fast retransmit. C# #1's scheme, runs of 16 to the least-loaded connection, is there
as `PG_RUN=16`; on our rig it gave a p99 of ~210 ms under 1% loss on the Postgres path, against ~63 ms for filling to
128, because short flights end in tail losses that wait for a timeout. Every statement is prepared once per
connection; parameters and results are binary, and rows are read in place from the receive buffer. A dropped
connection is reopened in the background while the others carry the load. A connection that owes answers and has
received nothing for 10 s is treated as dropped (as in C# #1): its queries fail at once instead of
waiting for the client's timeout. The Postgres sockets set `TCP_THIN_LINEAR_TIMEOUTS` too.

**The SQL is SPEC.md's, one statement per request.** The reads a thread sends on one connection in one pass share
a `Sync`; if one fails, Postgres skips the rest of that batch, which is sent again. The writes sent on one
connection in one pass share a `Sync` too, so Postgres commits them as one implicit transaction (group commit);
each write is answered only after that `Sync`'s `ReadyForQuery`. A failing like (SQLSTATE 23503) gets its 404 and
the others in the batch are sent again. Nothing is cached across requests, and every token's HS256 signature,
`alg` and `exp` are checked on every request (a token without `exp` is rejected).

**One wake-up for many events.** After a pass that did work, the thread waits in `io_uring_enter` for 128
completions or 4 ms, whichever comes first, so the requests and Postgres answers that arrive meanwhile share one
pass and one send per connection. An idle thread waits for one completion.

**TCP options on our own sockets.** The listening sockets clear `TCP_QUICKACK` (the answer carries the ACK of the
request) and set `TCP_THIN_LINEAR_TIMEOUTS` (no exponential backoff while few segments are in flight); accepted
sockets inherit both. These are per-socket options, not sysctls. `TCP_NODELAY` everywhere.

**Responses by hand**: compact JSON in the spec's key order, `created_at` from the binary `timestamptz` in UTC with
milliseconds, strings escaped. Bodies of 512 bytes or more are gzipped when the client accepts it.

**HTTP/1.1** keep-alive with no idle timeout, listen backlog 65535, pipelined requests answered in order,
`Expect: 100-continue`, `Connection: close`, HTTP/1.0 keep-alive.

## Settings

All optional; the defaults are what gets benchmarked.

| Variable | Default | |
|---|---|---|
| `THREADS` | `Environment.ProcessorCount` | I/O threads |
| `DB_READ_CONNS`, `DB_WRITE_CONNS` | 192, 64 | Postgres connections in total, split over the threads (at most 512) |
| `GZIP_IMPL` | `igzip` | `igzip` (ISA-L), `libdeflate` or `fsharp` (pure F# encoder) |
| `GZIP_LEVEL`, `GZIP_MIN` | 0 (igzip), 1 (libdeflate); 512 | compression level; smallest body that is gzipped |
| `PG_DEPTH` | 32 | queries in flight per Postgres connection before the next one is used; `0`: fewest in flight |
| `PG_RUN` | 0 | `16`: each pass deals its queries in runs of 16 to the least-loaded connection (C# #1's scheme; overrides `PG_DEPTH`) |
| `PG_BATCH_SYNC`, `PG_GROUP_WRITES` | 1, 1 | `0`: a `Sync` per read / per write |
| `PG_TIMEOUT_MS` | 10000 | a Postgres connection that owes answers and has received nothing this long is dropped and reopened; `0`: off |
| `PG_THIN` | 1 | `TCP_THIN_LINEAR_TIMEOUTS` on the Postgres sockets; `0`: off |
| `WAIT_N`, `WAIT_US` | 128, 4000 | after a busy pass, wait for this many completions or this long |
| `NBUFS` | 16384 | 2 KB receive buffers per thread (a power of two, at most 32768) |
| `REARM_ALL` | 1 | `0`: #35's budget for re-arming receives that found no buffer |
| `QUICKACK_OFF`, `THIN_LTO` | 1, 1 | listener TCP options above |
| `LEGACY_BUFS` | off | `1`: provided buffers through `IORING_OP_PROVIDE_BUFFERS` |
| `MUC_STATS` | off | `1`: per-thread counters every 5 s (passes, completions, ENOBUFS, longest pass, GC pause, CQ drops) |

## Correctness

`test/run.sh`: 42/42 on 4 and 8 cores, with the buffer ring, with `LEGACY_BUFS=1` and with `GZIP_IMPL=fsharp`.

## Credits

The Postgres client, JWT and I/O loop come from our earlier entries (#35, and before it the multicore challenge's
F# entry, whose Postgres client was ported from csharp-sockets-jh-d, MIT). Ideas from other entries, measured here
before keeping: separate read and write connections (rust-tokio-cknutson12), ISA-L igzip level 0 (#9, #12), a short
wait after a busy pass (#10, #11, #30), the `PBUF_RING` retry and group commit (#23), reads sharing one `Sync`
(#31), the listener TCP options (#65).

## Local results (NOT official)

Measured on a Synteq VPS smaller than the grader's (8 shared vCPUs, Xeon Platinum 8260, 16 GB, Ubuntu 24.04,
kernel 6.8.0-117, the grader's sysctls) with Postgres 18 and the load generators on the same box. **These are not
official numbers**; they only compare builds on the same rig. Details and every run in `experiments/worklog.md`.

- CPU per request, server on one vCPU (20,000 `mload` users at `-think-scale 0.25` plus 100,000 idle connections):
  ~69k cycles per request for this build, 65-71k for #35, 98-105k for C# #1 (#59) and 97-106k for Go #65.
- TCP segments per request on a 1500-byte MTU, clients and Postgres together: 4.20 (C# #1 4.26, Go #65 4.15).
- 1,000,000 keep-alive connections on 4 vCPUs, 5-minute hold at ~16.7k requests/s: no failures, p99 ~12-19 ms
  (measured before the Postgres defaults above, with 128 in flight per connection).
- Server on 2 vCPUs, 30,000 users (~11.5k requests/s) plus 100,000 idle connections: p95 17 ms, p99 42 ms, 0 errors.
