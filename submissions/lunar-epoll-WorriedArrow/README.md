# lunar-epoll: Lunar, one epoll loop per core, its own Postgres wire client

- Language: [Lunar](https://github.com/lunarln/lunar) 0.2.0, a compiled systems language with Rust-like syntax
  and no heap or garbage collector (LLVM backend). `lunar build --release` (LLVM optimizations on).
- Framework / IO: none; raw epoll through `extern "C"` declarations of libc, one loop per thread, hand-written
  HTTP/1.1
- Driver: none; the Postgres v3 wire protocol is spoken directly from `src/pg.ln` (SCRAM-SHA-256, Parse once,
  Bind/Execute per request, DataRow parsing)
- Compression: own gzip in Lunar (`src/gz.ln`, `src/huff.ln`): LZ77 with one hash probe per position and a
  Huffman code each thread trains from its first feeds
- Front: direct (no Nginx), ports `PORT` and `EXTRA_PORT`
- Postgres connections: 64 in all (`max(2, 64 / threads)` per thread)
- Dependencies: the Lunar 0.2.0 release (compiler, its standard library and LLVM), installed by `install.sh` from
  its official tarball with the SHA-256 checked, and the system's `cc` and glibc. No libpq, zlib or any other
  library: everything is in `src/` (about 4,300 lines of Lunar, formatted with `lunar fmt`).

## Why this design

It is the design of the top C entry, `c-epoll-unorig-username` (#10, #90, #96, #104, #100), carried over to Lunar
piece by piece, so the question this entry answers is whether a young language with no heap, no GC and an LLVM
backend gets the same numbers as C on the same plan. Measured locally it does (see below): the same server CPU
per request as the C entry, within run-to-run noise.

Lunar's own model shapes a few things:

- **Memory.** Lunar has no heap: every owning array lives on its own *data stack*, a reserved slot of address
  space, and at most 65,536 of them can be alive at once. A million connections cannot each own an array, so the
  per-connection state is one table of plain 40-byte structs indexed by descriptor (`calloc`'d once: untouched
  pages cost nothing), and the buffers that exist only while a request or response is split across reads or
  writes come from libc `malloc`. Everything on the request path goes through raw pointers in `unsafe` blocks;
  Lunar requires every unsafe operation to be inside one, even in an `unsafe fn`.
- **Threads.** `std::thread` (new in 0.2.0) starts one thread per CPU in the affinity mask. Per-thread state
  (the response buffer, the epoll set, the Postgres connections, the gzip tables) is `#[thread_local]`; the
  connection table is shared, but a descriptor is only ever touched by the thread that accepted it.
- **Strings.** Startup code (URL parsing, SCRAM, the startup and Parse messages) uses Lunar's `String`, `str`
  and growable arrays; the request path works on raw bytes.

## Design

- **Threads**: one per CPU in the affinity mask (`sched_getaffinity`), so `CPUAffinity`/`taskset` sets it.
  Nothing else is configurable.
- **Listeners**: every thread opens its own `SO_REUSEPORT` listener on **both** ports and accepts into its own
  epoll set; the kernel spreads new connections over the threads. A connection lives on its thread for its whole
  life, so threads share nothing on the request path and take no locks.
- **Postgres**: each thread owns its connections, non-blocking, driven by the same epoll loop. At connect:
  StartupMessage, authentication (trust, password or SCRAM-SHA-256, which the box uses), then one Parse per
  statement (exactly the SQL in `SPEC.md`, parameter types given) under one Sync. Per request: one Bind (unnamed
  portal, binary parameters, results binary except a post's `id` and `like_count`, which come back as text because
  "523117" and "0" are shorter than 8 and 4 bytes) and one Execute appended to the connection's output buffer; no
  Describe, so a reply is BindComplete, DataRows, CommandComplete and nothing else. Replies come back in order
  and are matched to their requests from a ring; the response is built straight from the DataRow bytes in the
  read buffer.
  - Reads: a loop pass's reads fill one connection's batch (up to 64) and share one Sync: one write, one round
    trip, one implicit transaction. The next batch goes to the least-loaded read connection.
  - Writes go to separate connections (a quarter of each thread's, at least one). A pass's writes form one group
    (up to 128) under one Sync: one implicit transaction, one commit (group commit). Every write's response is
    held until that Sync's ReadyForQuery, which Postgres sends after the commit (rule 8). If a write in the group
    fails (a like on a missing post, `23503`), Postgres rolls the group back and skips the rest: the failing
    write gets its own error (404), and the others are sent again, each alone between two Syncs.
  - Lost connections (socket error, EOF, a reply that makes no sense, or 10 s of silence while owing replies):
    outstanding reads are sent again on another read connection of the thread (at most 3 times per request),
    outstanding writes answer 500 since their commit may have run (a write is never sent twice). Reconnects are
    tried once a second per thread.
- **HTTP**: requests are parsed in place from a per-thread 64 KB read buffer. While a request waits for Postgres
  its connection is "busy"; pipelined requests behind it wait in a per-connection buffer, so responses stay in
  order. Responses are built in a per-thread buffer, `Content-Length` back-filled, and sent with one `writev`
  (headers + gzip body). Keep-alive connections are never timed out.
- **Batching window**: a loop pass lasts at least 2 ms from the moment `epoll_wait` returned; what is left is slept
  before the next `epoll_wait`, so one wake-up serves everything that arrived meanwhile, its reads share one Sync
  and its writes one commit. A pass that filled the 512-event array goes straight on.
- **Scheduling**: the loop threads put themselves in `SCHED_IDLE`, so on the same core the kernel's receive work
  (`ksoftirqd`) runs first. The thread's own policy; no affinity, nothing box-wide.
- **Sockets**: `TCP_DEFER_ACCEPT` on the listeners (a new connection arrives with its first request, served in
  the same pass), `TCP_QUICKACK` 0 on accepted sockets and again after a reply later than 30 ms (the request's ACK
  rides on the response), `TCP_THIN_LINEAR_TIMEOUTS` on client and Postgres sockets.
- **Validation**: strict JSON (RFC 8259), HS256 JWT with the signature and a required `exp` checked on every
  request (SHA-256 and HMAC in `src/crypto.ln`), body trimming as JavaScript's `String.trim`, ids as in the spec.

## gzip

`/feed` is the only body of 1 KB or more (5.1 KB of JSON); it is compressed when the request accepts gzip, smaller
bodies go out plain. The compressor is in Lunar:

- **LZ77**: a 4,096-entry table (8 KB, cleared per response) of 16-bit positions keyed by a hash of 4-byte
  prefixes, one probe per position (no chains, no lazy matching), matches extended 8 bytes at a time.
- **Huffman**: each thread starts with deflate's fixed code (2.4 KB per feed) and counts the literal, length and
  distance symbols of its first 64 feeds; it then builds its own length-limited code from those counts and the
  dynamic block header once (`src/huff.ln`), and every later response copies that header and codes with it:
  **2.0 KB per feed** (ISA-L level 0 gives about 2.2 KB). It counts again over 64 of every 4,096 feeds and
  rebuilds, so the code follows the feed as the posts in it change (in a load test the feed soon holds only the
  load generator's own posts). Every printable ASCII byte, length and distance always has a code; a body with other
  bytes (non-ASCII text) is coded with the fixed code instead. Nothing about the data is hard-coded: the code
  comes from the bodies this server sent.
- **Bit writer**: branchless, one 64-bit store per code, a match's length and distance codes in one write;
  codes are looked up from packed tables (code and length in one word; a length's extra bits precomputed per
  match length).
- **CRC-32**: slice-by-8 tables.
- Cost: about 45,000 cycles per 5.1 KB feed in this VM, measured the same as ISA-L level 0 in the C entry.

Either way a feed is two TCP segments.

## Optimizations, and why

| What | Why |
|---|---|
| One epoll loop per core, SO_REUSEPORT per thread | no locks or cross-thread hand-off on the request path |
| Own wire client: Parse once, Bind + Execute per request, no Describe | many requests per round trip; no SQL parsing or planning per request; integers and timestamps without text parsing; no RowDescription in any reply |
| Responses built from the DataRow bytes in place | no result objects, no per-row allocation or copy between the socket and the JSON |
| A pass's reads in one batch on one connection, one Sync | one write, one round trip and one ReadyForQuery per batch |
| Separate write connections, group commit | a commit (fsync on the database box) only delays other writes, never reads; one commit per pass |
| 2 ms pause after a light pass | fewer wake-ups, `epoll_wait` calls and Postgres round trips per request |
| 40-byte connection slot, no per-connection buffers | a million idle keep-alive connections cost ~40 MB of user memory |
| `created_at` formatted from the binary timestamp | truncates to milliseconds exactly, no time library |
| `id` and `like_count` as text | fewer bytes from Postgres per feed |
| `SCHED_IDLE` loop threads | the kernel's receive work on a core never waits behind the server |
| `TCP_DEFER_ACCEPT`, `TCP_QUICKACK` 0 | one wake-up per new connection; the request's ACK rides on the response |
| Own gzip with a trained Huffman code, only above 1 KB | 5.1 KB -> 2.0 KB per feed with one hash probe per byte; small bodies would not shrink |

## Rules notes

- Queries: the five statements are exactly the ones in `SPEC.md` (plus `SELECT 1` for `/health`), one per
  request, prepared, pipelined. Nothing is cached across requests: every response is built from its own query's
  rows, and every token is verified on every request.
- Re-sent reads: only when a connection is lost (error, EOF, or 10 s of silence), at most 3 times, and only one
  reply reaches the client; writes on a lost connection answer 500 and are never re-sent. The same rule as
  `c-epoll-unorig-username`, `fsharp-kestrel-uring-delneg` (#105) and `csharp-epoll-jh-d` (#59).
- Writes: group commit; every write's response is sent only after the Sync that commits its group (or, after a
  failed group, its own retry) has returned.
- gzip training uses only the bodies this server has sent; it changes how bytes are coded, never what is sent.
- `SCHED_IDLE` and `TCP_QUICKACK` are settings of this process's own threads and sockets.
- Connections: 64 to Postgres at most, well under 512.

## Measured locally

Docker on a 12-thread Windows desktop (WSL2 VM shared with other work), Postgres 18 in the same container with
`box/postgresql.conf` (shared_buffers 2 GB), pinned to 4 CPUs; server on 4 others; `mload` on the last 4, over
loopback. Treat absolute numbers as rough: the host is noisy. The useful comparisons are A/B against
`c-epoll-unorig-username` (the C entry this design comes from) built with its own `build.sh`, same harness, runs
alternating.

- `test/run.sh`: `passed=42 failed=0`, also in a clean `ubuntu:24.04` container that ran `install.sh` as root and
  `build.sh` as an unprivileged user (what CI does).
- gzip: 1,000 feeds with `Accept-Encoding: gzip`, each checked to decompress to the plain feed, before and after
  training: 0 bad. Sizes: seed feed 5,102 -> 2,435 bytes (fixed code) -> 1,980 bytes (trained); a feed of
  load-test posts 3,624 -> 761 bytes (one segment instead of three).
- `putjs` (JSON string escaping, 8 bytes at a time) is checked against the byte-at-a-time loop on 20,000 random
  strings full of quotes, backslashes, control and non-ASCII bytes (`lunar test --release`).
- mload, 150,000 users, `-gzip -think-scale 2.5` (the scoring settings), 90 s hold, server on 4 CPUs: all 150,000
  connections held, 0 errors, p95 14 ms, p99 206 ms, server RSS 12 MB, 0.43 cores at ~6,200 req/s.
- A/B against the C entry, mload 20,000 users at `-think-scale 0.5` (~4,200 req/s): server CPU over the hold 816
  ticks (Lunar) vs 809 (C), p95 4.2 ms both, 0 errors both.
- A/B against the C entry, mload 40,000 users at `-think-scale 0.5`, two rounds each, hardware counters on the
  server process (per request): user cycles 40-41k (Lunar) vs 33-36k (C), kernel cycles 130-136k vs 141-150k, so
  the same total within noise; the kernel's network work is ~80% of the server's cycles either way.
- Single endpoint, one thread, wrk: user instructions per request `/posts/1` 3.8k (Lunar) vs 3.3k (C); a gzipped
  feed costs about the same cycles as ISA-L level 0 in this VM.

## Running

```bash
sudo ./install.sh   # build-essential, curl, and Lunar 0.2.0 into /opt/lunar-0.2.0
./build.sh          # bin/server
DATABASE_URL=postgres://user:pass@host:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 ./start.sh
```
