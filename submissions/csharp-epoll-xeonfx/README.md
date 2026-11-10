# C# epoll

C# on .NET 10 (10.0.402), no web framework and no database driver: an HTTP/1.1 server on raw epoll and a Postgres
client on the wire protocol, both in this folder. Linux only. One package: System.IO.Hashing 10.0.13, for gzip's
CRC-32.

## How it works

- **Independent event loops.** Each loop thread has its own listening sockets on both ports (SO_REUSEPORT lets the
  kernel spread connections over them), its own clients and its own Postgres connections. With at least 4 vCPUs,
  one fewer loop leaves more scheduling room for kernel network processing. Loops share nothing, so
  there are no locks and no thread hops between reading a request and writing its answer.
- **epoll, level-triggered, called directly.** `accept4`, `recv` and `send` go straight to libc; `recv` and `send`
  skip the GC transition because the sockets never block. A connection is a small struct in an array indexed by its
  fd, so a million idle connections cost about 40 bytes each plus the kernel's socket buffers.
- **No allocations per request.** Requests are parsed in place from one receive buffer per loop. Responses are
  written into one buffer per loop, the body first and the head just before it, so each response leaves in one
  `send`. Only a request that arrives in pieces or a response the socket can't take at once borrows a pooled buffer.
- **A loop works in passes of 4 ms.** A pass serves everything that is ready, then the loop sleeps out the rest of
  the 4 ms before it looks again. At a million users a loop otherwise wakes for nearly every request and every
  Postgres reply. The initial 2 ms version reduced 56,000 context switches/s to 4,400 at 1.4M users. One wake-up
  serves all that arrived meanwhile, and the cores are left to the kernel's network work, which is what runs out first on a box
  whose NIC has one queue. A pass that fills its event list goes straight on. The cost is a few milliseconds a
  request, against limits of 500 and 1,000.
- **Postgres: a pass's reads are one batch.** They go to one read connection as Bind/Execute pairs under a single
  Sync: one packet out, one wake-up of one backend, one ReadyForQuery back. Past 64 reads the next connection takes
  over. The connection with the fewest reads waiting gets the pass, the first of them, so while the first ones keep
  up they carry every pass and the rest stay free (8 read connections a loop on 4 cores, 2 on 16; about 32 in all).
  Each read is answered as soon as its rows are in; if one fails, the reads after it in the batch are sent again.
  The spec's statements are prepared once. Parameters and rows are binary, so ids, counts and timestamps need no
  text parsing; `created_at` is formatted straight from microseconds.
- **Group commit for writes, on write connections of their own.** A pass's new posts and likes go out together as
  one transaction (one Sync, one commit) on a write connection that has no group out (4 a loop on 4 cores, 2 on 8
  and 16). While all of them wait for their commit, new writes queue and leave as one group with the pass that
  finds one free. So a commit that is slow, or whose packet was lost, holds up its own group and nothing behind it,
  and reads never wait for a commit. If a write fails (a like on a missing post), it gets its error and the rest
  of its group are sent again.
- **Recovery on silent Postgres connections.** A quiet connection with a lost segment can wait for its
  retransmission timeout. A Postgres connection that owes replies and has been silent for 50 ms gets a lone Sync,
  with exponential backoff until data arrives. The extra traffic can trigger earlier loss detection. A writer
  tracks each nudge separately, so its reply can never acknowledge another write group's commit. Every socket
  also uses `TCP_THIN_LINEAR_TIMEOUTS` to limit timeout backoff for small amounts of data in flight. Clients
  inherit it and `TCP_NODELAY` from the listener: no call per connection.
- **JWT checked on every request** with HMAC-SHA256 on the stack: signature first, then HS256 (the usual header's
  exact bytes need no decoding), exp, sub and username.
- **The feed is gzipped** when the request's Accept-Encoding takes gzip. Only a body that wouldn't fit in one TCP
  segment with its head is worth it, and that is the feed: about 3.6 KB, three segments, one gzipped. `Gzip.cs` writes
  it without allocating: one deflate block with the fixed Huffman codes and greedy matches from a hash table that is
  never cleared (each call's positions sit past the last call's). About 4 µs a feed on an M1, where zlib-ng takes 9,
  ISA-L (level 0) 6 and libdeflate (level 1) 12, because at this size their setup costs more than the compression.
- NativeAOT, workstation GC without background GC, invariant globalization. `FEED_JIT=1 ./build.sh` builds the JIT
  version instead (start.sh runs whichever is built).

Login to Postgres is SCRAM-SHA-256 (or trust). A dropped database connection fails what it owed with 500 (503 for
`/health`) and reconnects within a second.

## The code

- `Program.cs` starts the loops; `Loop.cs` is the loop itself and its passes, `Loop.Http.cs` its clients
  (accept, parse, respond), `Loop.Routes.cs` the routes, `Loop.Postgres.cs` the reads, write groups and database
  connections.
- `Pg.cs` is one Postgres connection on the wire protocol, `Scram.cs` its login.
- `Json.cs` writes the responses and reads a new post's body, `Gzip.cs` compresses the feed, `Jwt.cs` checks tokens,
  `Native.cs` holds the libc calls.
