# c-iouring-einhornimmond

C on an io_uring loop written on the system calls (`src/uring.h`, no liburing), PostgreSQL
through libpq in pipeline mode (Ubuntu 24.04's, 16.x), HS256 over libsodium, requests parsed by
[picohttpparser](https://github.com/h2o/picohttpparser) (the parser of h2o), JSON read by
[arnm](https://github.com/gradido/arnm) 0.8.3 and written by hand, gzip through ISA-L's igzip
or libdeflate. Built with zig 0.15.2, which fetches and compiles the pinned dependencies
(picohttpparser, libsodium, arnm) from source; libpq, libdeflate and ISA-L come from apt.
Needs Linux 6.1 or later.

It grew out of an earlier submission of ours on h2o's event loop (`c-h2o-einhornimmond`) in
two steps: first h2o's epoll loop, and with it LibreSSL and zlib, gave way to an io_uring loop;
then the database side, which had sent one statement per round trip, was rewritten to run
libpq's pipeline mode, batching the statements of a loop pass on one connection. The token
check, the JSON and the gzip are the files they were. The arrangement is the one of
gradido-unicorn's `fast-servers`: thread-per-loop, with the database driven from the loops
themselves.

## Shape

```text
16 loop threads      one io_uring each, made by its own thread (SINGLE_ISSUER, DEFER_TASKRUN),
                     one listening socket per port under SO_REUSEPORT (the kernel spreads the
                     connections over the loops), 150 s idle timeout. A loop never waits on
                     anything but its ring.
512 connections      dealt out over the loops, 32 each; libpq non-blocking and pipelined, the
                     connection's socket watched by the loop's ring next to the clients.
                     Prepared statements, up to 32 in flight per connection. DB_POOL_SIZE,
                     the most rule 5 allows by default.
```

**One call into the kernel per pass.** The loop enters the kernel once: that call hands over
every send and every poll it queued during the pass before and brings back every request,
every completed send and every readable database socket since. Under load the loop lets
completions collect for up to a millisecond first, so the cost of the call is shared by all
of them. A request costs no `epoll_wait`, `read` or `sendmsg` of its own.

**Accept and receive are multishot.** One accept stays armed per listener, into the ring's
own table of direct descriptors, and one receive per connection, which borrows its buffer
from a ring of 4,096 provided buffers of 2 KB when data arrives. An idle connection holds no
buffer: 48 bytes of the server's memory and the kernel's socket. A request is parsed where it
arrived and the buffer goes back to the kernel when the handler returns; only a request that
arrives in pieces, or ahead of the answer to the one before, is copied into memory of the
connection. When a loop's table is full the accept rests until a connection has closed; new
users wait in the backlog.

**The database on the loop.** A request that touches the database is parked on its loop: a
slot of the loop takes the job, its statement goes out on a connection of the loop, the
connection's socket is watched by the loop's ring, and when the rows are in, the same loop
renders and sends the answer. No thread but the loop touches the request, its job or its
connection: no queue between threads, no worker to wake, no context switch. All of a loop's
connections full (32 statements in flight each): the job waits in the loop's queue (128 per
connection, 4,096 slots per loop, sized to ride out a checkpoint); the queue full: 503 at
once, nothing of the work runs. A session the server ends is dialled again from the loop,
non-blocking (`PQresetStart`); the statements that were in flight on it are answered 500 -- a
read could be repeated, but whether a write committed before the session went is exactly what
cannot be known, so neither is.

**Pipelined.** The connections run in libpq's pipeline mode. The statements a pass puts on
one connection -- up to 8, on the connection with the least still in flight -- go out in one
send, closed by one sync, and their results come back in a read or two: at 42,000 requests a
second that is a third of the system calls the database side made with one statement per
round trip, and a third less system time. The sync is where the server commits the batch, one
implicit transaction, so a request is answered only when the sync's result is in, never on
its own statement's (rule 8). A batch one statement of which errors -- a like of a post that
is not there, the foreign key -- rolls back as a whole: that statement is answered and the
others, which did nothing, are sent once more on the next pass.

**Statements** are the ones in SPEC.md, verbatim, prepared once per connection and executed
with binary results: ids and timestamps arrive as integers, text as bytes, and the loop reads
them straight out of the `PGresult` libpq produced -- nothing is copied between the driver and
the response. A create is one `INSERT ... RETURNING` in its own transaction, so the row is
committed when the statement returns and before the response is written (rule 8). A like's
`ON CONFLICT DO NOTHING` says through its row count whether it was the first (201) or a repeat
(200); the foreign key on `post_id` says 404.

**A response leaves in one piece**, head and body in one send, from a buffer freed when the
send has completed. The constant error responses are literals, sent from where they lie.

**gzip** is done on the loop for bodies of 1 KB and more when the client sent
`Accept-Encoding: gzip`. Feeds shrink to about a quarter; a single post object would shrink by
a quarter and is sent as it is. Which library is a build-time choice (`build.zig`):

- **igzip** (ISA-L) when `/usr/include/isa-l/igzip_lib.h` is installed: level 0, which is one
  pass with a fixed Huffman table, and the table is ours. It is built from a response the loop
  has just sent and rebuilt about once a second, so it follows what the users are posting
  without anybody knowing in advance what that is. A histogram is statistics, not data; no
  response is kept (rule 7).
- **libdeflate** otherwise, level 1, one compressor per loop. The build says which it took.

Measured on the body shapes of the load profile (feeds of load-generator posts, 3.6 KB), one
response, Ryzen 7 5700G, AVX2 (igzip has an AVX-512 path for the Xeon; not measured here):

| | ns/response | ratio | cores at 42,000 req/s |
|---|---:|---:|---:|
| zlib q1, stream kept | 13,600 | 4.26x | 0.57 |
| libdeflate q1 | 9,100 | 4.38x | 0.38 |
| igzip L1 | 6,900 | 4.25x | 0.29 |
| igzip L0, table retrained from the previous response | 3,000 | 4.10x | 0.13 |

**Connections.** Each user holds a keep-alive connection; the service gets 2 M descriptors.
The sockets in a loop's table do not count against that limit, but the limit is what the
service was given for connections, so the loops share it (with room for the kernel spreading
them unevenly: twice the limit over the loops, a power of two, at most 1 M per loop). An idle
socket costs about 3.9 KB in the kernel and 48 bytes in the server, so a million of them are
about 4 GB.

**JWT.** HS256 only. The HMAC state after the key is derived once; per token what is left is
the hash over the token, the constant-time compare, and reading `sub`, `username` and `exp`.
Nothing about a token is remembered between requests.

## Results

Measured locally with `bench/cload` (the challenge's load generator in C, see
`bench/README.md`): server, cload and a Postgres 15 with 64 connections on one 32-thread
Ryzen 5950X, loopback, gzip, two ports; the server pinned to four cores (`taskset -c 0-3`,
`SERVER_THREADS=4`), cload on the others. The same runs against the h2o version this grew out
of, and against this server before the database was pipelined, are in `bench/README.md`.

| users | req/s | result | server |
|---|---:|---|---|
| 100,000, think-scale 0.25 | 42,000 | pass, p99 8.6 ms, 0 fails | 1.0 cores (user 0.4, sys 0.7), 19 MB |
| 1,000,000, think-scale 2.5 | 42,000 | pass, p99 8.5 ms, max 88 ms, 0 fails | 1.1 cores (user 0.4, sys 0.7), 149 MB |

Against the h2o version with the same per-loop libpq, one statement per round trip (1.7
cores, user 0.6, sys 1.1, 1.68 GB at a million users), the io_uring loop saves a third of
the user time and all but 149 MB of the memory: an idle connection holds no buffer.
Pipelining the database took the system time from 1.1 to 0.7 cores (one statement per round
trip: 1.5 cores in all, p99 7 ms); what is left is the TCP stack serving 42,000 requests a
second. Batches of 8 were the knee: 2 per connection cost 1.2 cores, 16 saved nothing more.

## Rules

- Every response reads its data from Postgres while it is served. No caches of any kind;
  connection pools and prepared statements only.
- Writes are committed before the response: the batch's sync commits it, and the response
  waits for the sync's result; `synchronous_commit` as the server has it.
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
src/uring.h  the ring on the system calls: setup, enter, direct descriptors, provided buffers
src/server.c the loops, two listeners per loop, routing, rendering, parked requests
src/db.c     the per-loop connection pools and the four statements, driven by the host's poll
src/gz.c     igzip or libdeflate behind one function
src/jwt.c    HS256 verification
```

`SERVER_THREADS` (default: one loop per core) and `DB_POOL_SIZE` are the two knobs.

## Load generator

`bench/cload` is the challenge's `mload` in C (see `bench/README.md`): same user loop, same
result schema, usable with `mload -merge`, about 250 bytes per idle user instead of 30 KB, so
one box runs a quarter million users on a few MB.

## License

MIT, under the [license](../../LICENSE) of the repository.
