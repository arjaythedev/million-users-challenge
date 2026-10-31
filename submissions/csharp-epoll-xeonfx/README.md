# C# epoll

C# on .NET 10 (10.0.402), no web framework and no database driver: an HTTP/1.1 server on raw epoll and a Postgres
client on the wire protocol, both in this folder. Linux only.

## How it works

- **One event loop per core.** Each loop thread has its own listening sockets on both ports (SO_REUSEPORT lets the
  kernel spread connections over them), its own clients and its own Postgres connections. Loops share nothing, so
  there are no locks and no thread hops between reading a request and writing its answer.
- **epoll, level-triggered, called directly.** `accept4`, `recv` and `send` go straight to libc; `recv` and `send`
  skip the GC transition because the sockets never block. A connection is a small struct in an array indexed by its
  fd, so a million idle connections cost about 40 bytes each plus the kernel's socket buffers.
- **No allocations per request.** Requests are parsed in place from one receive buffer per loop. Responses are
  written into one buffer per loop, the body first and the head just before it, so each response leaves in one
  `send`. Only a request that arrives in pieces or a response the socket can't take at once borrows a pooled buffer.
- **Postgres: a few connections, pipelined.** About 32 read connections in all (8 per loop on 4 cores, 2 on 16) and
  one write connection per loop, far under the 512 limit. The spec's statements are prepared once. Reads are
  pipelined: reads go to one connection as Bind/Execute pairs held open for 1 ms (or 32 reads), then sent under a
  single Sync, so the reads of several loop passes leave in one packet and Postgres answers them with one
  ReadyForQuery and one flush. The loop sleeps in `epoll_pwait2` until the oldest held read is due. Each read is
  answered as soon as its rows are in; if one fails, the reads after it in the batch are sent again. Parameters and
  rows are binary, so ids, counts and timestamps need no text parsing; `created_at` is formatted straight from
  microseconds.
- **Group commit for writes.** New posts and likes that arrive within 1 ms, or while a write is in flight, go out
  together as one transaction (one Sync, one commit). If one fails (a like on a missing post), it gets its error and
  the rest are sent again.
- **JWT checked on every request** with HMAC-SHA256 on the stack: signature first, then HS256 (the usual header's
  exact bytes need no decoding), exp, sub and username.
- NativeAOT, workstation GC without background GC, invariant globalization. `FEED_JIT=1 ./build.sh` builds the JIT
  version instead (start.sh runs whichever is built).

Login to Postgres is SCRAM-SHA-256 (or trust). A dropped database connection fails what it owed with 500 (503 for
`/health`) and reconnects within a second. Responses are not gzipped: a gzipped feed costs more CPU than its two saved
segments.

## The code

- `Program.cs` starts one loop per core; `Loop.cs` is the loop itself, `Loop.Http.cs` its clients (accept, parse,
  respond), `Loop.Routes.cs` the routes, `Loop.Postgres.cs` the reads, write groups and database connections.
- `Pg.cs` is one Postgres connection on the wire protocol, `Scram.cs` its login.
- `Json.cs` writes the responses and reads a new post's body, `Jwt.cs` checks tokens, `Native.cs` holds the libc calls.
