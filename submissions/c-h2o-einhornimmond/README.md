# c-h2o-einhornimmond

C on h2o's event loop (libh2o-evloop, pinned commit `706842c`), PostgreSQL through libpq in
pipeline mode, HS256 over libsodium, JSON read by arnm 0.8.3 and written by additional code
of the server, gzip through ISA-L's igzip or libdeflate. Built with zig 0.15.2, which fetches
and compiles the pinned dependencies (h2o, LibreSSL, zlib, libsodium, arnm) from source;
libpq, libdeflate and ISA-L come from apt. The arrangement is the one of gradido-unicorn's servers:
one thread per loop, with the database driven from the loops themselves.

## How it works

- **One loop per core.** Each loop has its own h2o context and evloop, its own listening
  socket per port under `SO_REUSEPORT` (the kernel spreads the connections over the loops) and
  its own share of the database connections. Nothing crosses a thread: no queue between
  threads, no worker to wake, no context switch per request.
- **The database on the loop.** A request that needs rows is parked in a slot of its loop; its
  statement goes out on a connection of the loop, the connection's socket sits in the loop's
  epoll next to the clients, and when the rows are in, the same loop renders and sends the
  answer. All connections full: the job waits in the loop's queue; the queue full: 503 at
  once. A session the server drops is dialled again from the loop, non-blocking; what was in
  flight on it is answered 500, because whether a write committed cannot be known.
- **Pipelined.** The connections run in libpq's pipeline mode. The statements a loop puts on
  one connection within a millisecond -- up to 8 -- go out in one send, closed by one sync,
  and come back in a read or two. The sync is where the server commits the batch, one
  implicit transaction, so a request is answered only when the sync's result is in (rule 8). A
  batch one statement of which errors rolls back as a whole: that statement is answered (404
  for a like of a post that is not there) and the others are sent again. The millisecond the
  sync waits is what makes the batches, and what a request pays in latency; it cut a quarter
  of the server's system time and a quarter of Postgres's CPU.
- **Statements** are the ones in SPEC.md, verbatim, prepared once per connection, with binary
  results read straight out of the `PGresult` into the response. A like's `ON CONFLICT DO
  NOTHING` says through its row count whether it was the first (201) or a repeat (200).
- **gzip** on the loop for bodies of 1 KB and more when the client accepts it. igzip (ISA-L)
  at level 0 with a Huffman table built from a response the loop has just sent and rebuilt
  about once a second: 3 µs per feed, which shrinks to a quarter. Without ISA-L the build
  takes libdeflate at level 1: 9 µs per feed. h2o's own compress filter would cost 76 µs. No
  response is kept for it, only a histogram (rule 7).
- **Connections.** Each user holds a keep-alive connection. An idle one costs about 3.9 KB in
  the kernel and 2 to 3 KB in h2o, no buffer, so a million are under 8 GB. The idle timeout is
  150 s. Out of descriptors, the loop pauses its listeners for 100 ms; new users wait in the
  backlog.
- **JWT.** HS256 only, the HMAC state after the key derived once; nothing about a token is
  remembered between requests.

## Results

The scoring runs of the challenge, as near as one box allows: a million users at the
challenge's profile (`-think-scale 2.5`, 42,000 requests a second), 7 minutes of ramp and a
5-minute hold, the server on 16, 8 and 4 cores of a Ryzen 5950X with one loop per core, the
load generator (`bench/cload`) on the other 16 threads, a local Postgres 15 with 64
connections. CPU over 200 s of the hold:

| cores | result | server | RSS |
|---:|---|---|---|
| 16 | pass, p99 7.4 ms, max 157 ms, 0 errors | 1.8 cores (user 0.75, sys 1.09) | 1.9 GB |
| 8 | pass, p99 8.3 ms, max 119 ms, 0 errors | 1.9 cores (user 0.79, sys 1.11) | 1.8 GB |
| 4 | pass, p99 8.7 ms, max 112 ms, 0 errors | 1.5 cores (user 0.64, sys 0.87) | 1.8 GB |

The worst 5-second window of any hold had a p99 of 59 ms; the challenge allows 1 s. With
fewer loops a pass of the loop holds more statements and responses, so the batches are fuller
and the server needs less. Postgres took 4 to 5 cores on the same box.

## Rules

- Every response reads its data from Postgres while it is served. No caches; connection pools
  and prepared statements only.
- Writes are committed before the response: the batch's sync commits, the response waits for
  the sync's result; `synchronous_commit` as the server has it.
- The schema and the statements are the spec's. No work is moved into Postgres.
- At most `DB_POOL_SIZE` (512) connections. A server that refuses one keeps the pool at what
  it gave, and the startup line says how many.
- Nothing on the box is touched; the process raises its own soft `RLIMIT_NOFILE` to the hard
  limit and nothing else.

## Files

```text
install.sh   zig 0.15.2 (pinned, sha256-checked), libpq-dev, libdeflate-dev, libisal-dev
build.sh     zig build --fetch; zig build -Doptimize=ReleaseFast
start.sh     bin/server, DB_POOL_SIZE defaulting to 512
src/main.c   environment, RLIMIT_NOFILE
src/server.c h2o: loops, two listeners per loop, routing, rendering, parked requests
src/db.c     the per-loop connection pools, pipelined, and the four statements
src/gz.c     igzip or libdeflate behind one function
src/jwt.c    HS256 verification
bench/       cload, the challenge's load generator in C (see bench/README.md)
```

`SERVER_THREADS` (default: one loop per core) and `DB_POOL_SIZE` are the two knobs.

## License

MIT, under the [license](../../LICENSE) of the repository.
