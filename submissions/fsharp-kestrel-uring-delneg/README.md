# fsharp-kestrel-uring-delneg: F# on raw io_uring, no framework, its own HTTP and Postgres code

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

This entry first ran its io_uring loop under Kestrel's host. That host is gone now: the server is a plain console app
that runs its own HTTP/1.1 and Postgres code on raw io_uring. The folder keeps its old name so the entry is scored in
place.

## Design

**One I/O thread per core, nothing shared.** `Environment.ProcessorCount` threads (it follows the affinity mask the
box gives the process, so 16, 8 and 4 cores size themselves). Each thread has its own io_uring ring
(`SINGLE_ISSUER | DEFER_TASKRUN | COOP_TASKRUN`), its own `SO_REUSEPORT` listening socket on each port and its own
Postgres connections. No locks and no hand-offs between threads on the request path.

**An idle connection owns no buffer.** Accepted sockets go straight into the ring's fixed-file table (multishot
accept with `IORING_FILE_INDEX_ALLOC`), so they never get a process fd. Each socket has one multishot recv that
takes a 2 KB buffer from the thread's provided-buffer ring (8,192 of them, 16 MB) only when bytes arrive. The request is parsed in place
and the buffer goes straight back; only a request split over several receives is copied. Per-connection state is
a few entries in arrays indexed by the fixed-file slot.

**Receives that find no buffer are all re-armed.** When a burst empties the buffer ring (after a long pause, say),
the multishot receives that got `ENOBUFS` end and must be armed again. The Kestrel version re-armed at most 32 of them per loop
pass. Once every buffer is back in the ring at the end of a pass, this entry re-arms all of them. On our rig, with
buffers made scarce on purpose (`NBUFS=64`), the old budget turned a Postgres stall into a p95 of 2.1 s at 1M
connections where re-arming all gave 0.3 s.

**Ubuntu 6.8 kernels and buffer rings.** Ubuntu's 6.8.0 kernels -132, -135..-142 and -145..-149 refuse
`IORING_REGISTER_PBUF_RING` with `EINVAL` (the reserved-field check is inverted, Launchpad #2162843). The server
registers normally first; on `EINVAL` it retries once with `resv[0] = 1`, which those kernels accept; if that fails
too it falls back to `IORING_OP_PROVIDE_BUFFERS`. `LEGACY_BUFS=1` forces the fallback (42/42 that way too).

**`/health` waits for every thread.** It answers 503 until each I/O thread has at least one read and one write
connection, so a load that starts on the first 200 never reaches a thread that is still connecting.

**Postgres: the connection layout the Kestrel version was scored with.** Each thread opens its share of 64 read and
32 write connections (4 cores: 16 + 8 per thread). A query goes to the first connection of its kind with fewer than
128 queries in flight, so the queries of one wake-up leave in one send and wake few backends. A shallower layout
(192 + 64 connections at depth 32) gave better tails under loss on our loopback rig and a lower 16-core p95 on the
grader (40 ms against 216 ms), but held 1,240,000 on 4 cores against 1,340,000 for this one, so it is off by
default (`DB_READ_CONNS=192 DB_WRITE_CONNS=64 PG_DEPTH=32`). C# #1's scheme, runs of 16 to the least-loaded
connection, is there as `PG_RUN=16`. Every statement is prepared once per
connection; parameters are binary, and so are results except ids and like counts, which come as text (fewer bytes
than 8- and 4-byte integers, and their digits go into the JSON as they are, as in Go #65). Rows are read in place
from the receive buffer. A dropped
connection is reopened in the background while the others carry the load. Its reads go to another read connection
(as C# #1 does; at most 3 times per request); its writes get a 500, since they may have committed. With the 40
busiest backends terminated under load, that took the 500s from 572 to 33. A connection that owes answers and has
received nothing for 10 s is treated as dropped (as in C# #1): its queries fail at once instead of
waiting for the client's timeout. `PG_THIN=1` sets `TCP_THIN_LINEAR_TIMEOUTS` on the Postgres sockets.

**The SQL is SPEC.md's, one statement per request.** The reads a thread sends on one connection in one pass share
a `Sync`; if one fails, Postgres skips the rest of that batch, which is sent again. The writes sent on one
connection in one pass share a `Sync` too, so Postgres commits them as one implicit transaction (group commit);
each write is answered only after that `Sync`'s `ReadyForQuery`. A failing like (SQLSTATE 23503) gets its 404 and
the others in the batch are sent again. Nothing is cached across requests, and every token's HS256 signature,
`alg` and `exp` are checked on every request (a token without `exp` is rejected).

**One wake-up for many events.** After a pass that did work, the thread waits in `io_uring_enter` for 128
completions or 4 ms, whichever comes first, so the requests and Postgres answers that arrive meanwhile share one
pass and one send per connection. An idle thread waits for one completion.

**TCP options on our own sockets.** The listening sockets set `TCP_DEFER_ACCEPT` (1 s), so a new connection reaches `accept` with its first
request already queued and the receive armed after it completes at once. `QUICKACK_OFF=1` clears `TCP_QUICKACK`
and `THIN_LTO=1` sets `TCP_THIN_LINEAR_TIMEOUTS` on them (accepted sockets inherit both); both are off by default,
as in the scored Kestrel version. These are per-socket options, not sysctls.
`TCP_NODELAY` everywhere.

**Responses by hand**: compact JSON in the spec's key order, `created_at` from the binary `timestamptz` in UTC with
milliseconds, strings escaped. Bodies of 512 bytes or more are gzipped (igzip level 0) when the client accepts it. `GZIP_TRAIN=1` rebuilds
igzip's Huffman table about once a second from a live body's symbols (#9, #12), which takes a feed from ~2,240 to
~1,960 bytes; it is off by default, as in the scored Kestrel version.

**HTTP/1.1** keep-alive with no idle timeout, listen backlog 65535, pipelined requests answered in order,
`Expect: 100-continue`, `Connection: close`, HTTP/1.0 keep-alive.

## Settings

All optional; the defaults are what gets benchmarked.

| Variable | Default | |
|---|---|---|
| `THREADS` | `Environment.ProcessorCount` | I/O threads |
| `DB_READ_CONNS`, `DB_WRITE_CONNS` | 64, 32 | Postgres connections in total, split over the threads (at most 512) |
| `GZIP_IMPL` | `igzip` | `igzip` (ISA-L), `libdeflate` or `fsharp` (pure F# encoder) |
| `GZIP_LEVEL`, `GZIP_MIN` | 0 (igzip), 1 (libdeflate); 512 | compression level; smallest body that is gzipped |
| `PG_DEPTH` | 128 | queries in flight per Postgres connection before the next one is used; `0`: fewest in flight |
| `PG_RUN` | 0 | `16`: each pass deals its queries in runs of 16 to the least-loaded connection (C# #1's scheme; overrides `PG_DEPTH`) |
| `PG_BATCH_SYNC`, `PG_GROUP_WRITES` | 1, 1 | `0`: a `Sync` per read / per write |
| `PG_TIMEOUT_MS` | 10000 | a Postgres connection that owes answers and has received nothing this long is dropped and reopened; `0`: off |
| `PG_TEXT_IDS` | 1 | ids and like counts as text result columns; `0`: all binary |
| `GZIP_TRAIN` | 0 | igzip: Huffman table rebuilt about once a second from a live body; `0`: igzip's default table |
| `PG_THIN` | 0 | `1`: `TCP_THIN_LINEAR_TIMEOUTS` on the Postgres sockets |
| `WAIT_N`, `WAIT_US` | 128, 4000 | after a busy pass, wait for this many completions or this long |
| `NBUFS` | 8192 | 2 KB receive buffers per thread (a power of two, at most 32768) |
| `REARM_ALL` | 1 | `0`: the old budget for re-arming receives that found no buffer |
| `QUICKACK_OFF`, `THIN_LTO` | 0, 0 | listener TCP options above |
| `DEFER_ACCEPT` | 1 | `TCP_DEFER_ACCEPT` seconds on the listeners; `0`: off |
| `LEGACY_BUFS` | off | `1`: provided buffers through `IORING_OP_PROVIDE_BUFFERS` |
| `MUC_STATS` | off | `1`: per-thread counters every 5 s (passes, completions, ENOBUFS, longest pass, GC pause, CQ drops) |

## Correctness

`test/run.sh`: 42/42 on 4 and 8 cores, with the buffer ring, with `LEGACY_BUFS=1` and with `GZIP_IMPL=fsharp`.

## Credits

The Postgres client, JWT and I/O loop come from the Kestrel version of this entry, and before it from our F# entry
in the multicore challenge, whose Postgres client was ported from csharp-sockets-jh-d (MIT). Ideas from other entries, measured here
before keeping: separate read and write connections (rust-tokio-cknutson12), ISA-L igzip level 0 (#9, #12), a short
wait after a busy pass (#10, #11, #30), the `PBUF_RING` retry and group commit (#23), reads sharing one `Sync`
(#31), the listener TCP options (#65).

## Local results (NOT official)

Measured on a Synteq VPS smaller than the grader's (8 shared vCPUs, Xeon Platinum 8260, 16 GB, Ubuntu 24.04,
kernel 6.8.0-117, the grader's sysctls) with Postgres 18 and the load generators on the same box. **These are not
official numbers**; they only compare builds on the same rig.

- CPU per request, server on one vCPU (20,000 `mload` users at `-think-scale 0.25` plus 100,000 idle connections):
  ~69k cycles per request for this build, 65-71k for the Kestrel version, 98-105k for C# #1 (#59) and 97-106k for Go #65.
- TCP segments per request on a 1500-byte MTU, clients and Postgres together: 4.20 (C# #1 4.26, Go #65 4.15).
- 1,000,000 keep-alive connections on 4 vCPUs, 5-minute hold at ~16.7k requests/s: no failures, p99 ~12-19 ms
  (measured before the Postgres defaults above, with 128 in flight per connection).
- Server on 2 vCPUs, 30,000 users (~11.5k requests/s) plus 100,000 idle connections: p95 17 ms, p99 42 ms, 0 errors.
