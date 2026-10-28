# csharp-epoll-aot-gichamba

The feed API in C# with no web framework and no Postgres driver: our own HTTP/1.1 server on epoll and our own Postgres
client, compiled with Native AOT. [`csharp-epoll-jit-gichamba`](../csharp-epoll-jit-gichamba) is the same source on
the .NET JIT.

| | |
|---|---|
| Language | C# 14, .NET 10 (SDK 10.0.303, pinned by `global.json`), Native AOT |
| HTTP | our own HTTP/1.1 server on edge-triggered epoll, keep-alive and pipelining |
| Postgres | our own client for protocol 3.0: extended query, binary results, pipelined, SCRAM-SHA-256 |
| gzip | off by default; `GZIP_MIN` turns on our own encoder: one deflate block with the fixed Huffman codes, greedy matches; CRC-32 from Ubuntu's `libdeflate0` |
| Packages | none beyond the .NET SDK |

## How a request flows

1. One event loop per core, and nothing shared between loops. Each loop has its own `SO_REUSEPORT` listener on port 80
   and on port 81, so the kernel spreads new connections over the loops, and its own Postgres connections, which sit in
   its epoll set next to its clients. A loop accepts no clients until all its Postgres connections are open, so at
   startup `/health` doesn't answer 200 on one loop and 503 on another. If Postgres isn't up within 10 s, the loops start
   accepting anyway.
2. The loop reads, parses and checks the request: route, id and, on writes, the JWT and the JSON body. Bad requests
   are answered there from prebuilt responses.
3. A good request becomes one prepared statement on one of the loop's Postgres connections. Reads and writes use
   separate connections, so a read never waits behind a commit.
4. **Reads are packed.** The reads a loop pass puts on one connection, up to `PG_PACK`, go out as their Bind and
   Execute messages and one Sync, in one send. Each read is answered as soon as its own result arrives.
5. **Writes commit in groups.** A write connection runs one transaction at a time. The writes that arrive while it
   commits go out together as the next one: one Sync, one commit. Each response waits for the commit that holds its
   write. If one write in a group fails (a like on a missing post), the group rolls back and each write runs again on
   its own, so each gets the answer it would get alone.
6. The same loop gets the binary rows, writes the JSON straight into the response buffer and sends it. **It does not
   gzip by default.** SPEC.md makes compression optional, and on a 4-vCPU VM sending a 5.1 KB feed as four TCP segments
   cost 11% less CPU per request than compressing it to two. `GZIP_MIN` turns gzip on for bodies of at least that size.
   The encoder uses deflate's fixed Huffman codes: a feed comes out at about 2.4 KB instead of libdeflate's 1.9 KB,
   still two TCP segments, for less than half the CPU.
7. **A quiet loop waits a little.** After a pass that handled fewer than `COALESCE_MIN` events, the loop sleeps
   `COALESCE_US` before it looks again, so one wake-up serves the requests and results that arrived meanwhile.

## How each rule is met

- **The SQL is SPEC.md's**, one statement per request, prepared once per connection. Reads are pipelined; writes are
  batched into one transaction, which rule 3 allows.
- **No caching.** Every request runs its query. The only state kept between requests is connections, prepared
  statements, buffers and each thread's HMAC key state (the key, not any token).
- **Writes are committed before the response.** A write is answered only after the Sync result of its transaction,
  with `synchronous_commit` left on.
- **JWT.** Every request with a token checks the HS256 signature in constant time, `alg`, `exp`, `sub` and
  `username`.
- **At most 512 Postgres connections.** 96 by default; the server refuses to start with more than 512.
- **Ports 80 and 81** from `PORT` and `EXTRA_PORT`. Idle connections are never closed by the server.

## Millions of connections

An idle connection holds no buffers. Requests are parsed in the loop's shared receive buffer, and a connection rents a
buffer from a pool only while it has bytes waiting to be parsed or sent. What stays is a small object per connection
and the kernel's socket. On our rig the server's cgroup used 615 MB with 250,000 connections open, socket buffers
included. At that rate the 3,000,000 connections of the 8- and 4-core searches take about 7.5 GB of the 32.

Running out of descriptors does not end the process:

- `start.sh` raises the soft descriptor limit to the hard one.
- Clients never get the last 256 descriptors, which stay free for Postgres reconnects and the runtime. A connection
  that would take one is closed at once, and accepting pauses for 100 ms, as it does when accept fails with EMFILE or
  ENFILE. The open connections keep being served.
- The console is opened at startup, so writing an error later needs no new descriptor.
- A request whose handler throws gets a 500, and a connection whose event fails is closed. The loop carries on.

## Measured

On our own rig: Hyper-V VMs on a desktop i7-10700, the app VM set up with `box/setup-app-box.sh`, and Postgres 18 and
`bench/mload` on a second VM. That Postgres ran with `synchronous_commit = off` because its virtual disk takes 32 ms
per fsync; the server never changes the setting.

- On **1 vCPU** the JIT build passed 38,963 requests per second (100,000 users, think time cut to 0.27), 93% busy,
  p95 57 ms, p99 147 ms, no errors. That is about 24 µs of CPU per request, kernel included. At the scoring think time
  it is the rate of about 930,000 users.
- On **4 vCPUs** the rig ran out of load at about 47,000 requests per second. An earlier, costlier build was about
  71% busy there.
- Native AOT used about 2 µs more per request than the JIT on an earlier LAN rig.

These are our rig's numbers, not the scoring box's.

## Settings

Defaults are in `src/Settings.cs`; set any of these in the environment to experiment.

| Variable | Default | |
|---|---|---|
| `HTTP_LOOPS` | cores | event loops, each listening on both ports |
| `PG_READ` | 64 | read connections in all, split over the loops |
| `PG_WRITE` | 32 | write connections in all, split over the loops |
| `PG_PACK` | 32 | most reads behind one Sync on one connection in a pass |
| `COALESCE_MIN` | 32 | a pass with fewer events than this is followed by a wait |
| `COALESCE_US` | 2000 | that wait, in microseconds; 0 turns it off |
| `GZIP_MIN` | 0 | smallest body gzipped for a client that accepts gzip; 0 turns gzip off |
| `STATS_SECONDS` | 10 | seconds between stats lines on stderr |

## Build

- `install.sh` (root, once): clang, zlib (Native AOT links it), libdeflate, ICU and OpenSSL from apt, and the .NET SDK 10.0.303 from Microsoft's
  tarball, checked against its published SHA-512, into `/usr/local/lib/dotnet-sdk-10.0.303`. It waits for the apt
  lock, retries downloads, and is quick and safe to run again.
- `build.sh` (normal user): publishes the Native AOT binary to `out/`.
- `start.sh`: raises the soft descriptor limit to the hard one and runs `out/feed` in the foreground.

## Credits

The design is the one the leading entries share, and several ideas are taken straight from them:

- [`rust-tokio-cknutson12`](../rust-tokio-cknutson12) (#1): a thread per core with its own `SO_REUSEPORT` listeners
  and Postgres connections, nothing shared, reads apart from writes, and no buffer for an idle connection.
- `java-epoll-stohirov` (#2): group commit, retrying a failed group one write at a time, and wake-up batching after a
  pass of fewer than 32 events, for 2 ms.
- `zig-iouring-harshitkhandelwal208` (#4): packs of up to 32 reads behind one Sync (`PG_PACK`), and the names of the
  coalescing settings.
- `c-epoll-unorig-username` (#10): one Sync for a loop pass's reads, and the 2 ms pause after a light pass.

The HTTP server and Postgres client come from our multicore challenge entry,
[arjaythedev/multicore-server-challenge#103](https://github.com/arjaythedev/multicore-server-challenge/pull/103).
