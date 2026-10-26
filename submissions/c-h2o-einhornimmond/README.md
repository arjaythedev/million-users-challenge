# c-h2o-einhornimmond

C on [h2o](https://github.com/h2o/h2o)'s event loop (libh2o-evloop, pinned commit `706842c`),
PostgreSQL through libpq (Ubuntu 24.04's, 16.x), HS256 over libsodium, JSON read by
[arnm](https://github.com/gradido/arnm) 0.8.3 and written by hand (claude), gzip through ISA-L's igzip
or libdeflate. Built with zig 0.15.2, which fetches and compiles the pinned dependencies
(h2o, LibreSSL, zlib, libsodium, arnm) from source; libpq, libdeflate and ISA-L come from apt.

It is the arrangement of gradido-unicorn's `fast-servers`,
thread-per-loop on h2o, with the database driven from the loops themselves.

## Shape

```text
16 loop threads      one h2o context and evloop each, one listening socket per port under
                     SO_REUSEPORT (the kernel spreads connections over the loops), 150 s idle
                     timeout. A loop never waits on anything but its sockets.
512 connections     dealt out over the loops, 32 each; libpq non-blocking and pipelined, the
                     connection's socket in the loop's epoll next to the clients. Prepared
                     statements, up to 32 in flight per connection. DB_POOL_SIZE, the most
                     rule 5 allows by default.
```

A request that touches the database is parked on its loop: a slot of the loop takes the job,
its statement goes out on a connection of the loop, and when the rows are in, the same loop
renders and sends the answer. No thread but the loop touches the request, its job or its
connection: no queue between threads, no worker to wake, no context switch -- which halved the
server's CPU per request against the worker-pool version it replaced (see `bench/README.md`).
A tracker in the request's pool tells the slot when the client has gone, so a write still
commits and the answer goes nowhere. All of a loop's connections full (32 statements in flight
each): the job waits in the loop's queue (128 per connection); the queue full: 503 at once,
nothing of the work runs. A session the server ends is dialled again from the loop,
non-blocking (`PQresetStart`); the statements that were in flight on it are answered 500 -- a
read could be repeated, but whether a write committed before the session went is exactly what
cannot be known, so neither is.

**Pipelined.** The connections run in libpq's pipeline mode. The statements a loop puts on one
connection within a millisecond -- up to 8, on the connection with the least still in flight
-- go out in one send, closed by one sync, and their results come back in a read or two. An
iteration of h2o's loop rarely holds more than one statement per connection, so the sync waits
a millisecond for company (a timer); that is what makes the batches, and what a request pays
in latency. The sync is where the server commits the batch, one implicit transaction, so a
request is answered only when the sync's result is in, never on its own statement's (rule 8).
A batch one statement of which errors -- a like of a post that is not there, the foreign key
-- rolls back as a whole: that statement is answered and the others, which did nothing, are
sent once more. Against one statement per round trip this took a quarter off the server's
system time and as much off Postgres's CPU, which commits and flushes once per batch instead
of once per statement (`bench/README.md`).

**Statements** are the ones in SPEC.md, verbatim, prepared once per connection and executed
with binary results: ids and timestamps arrive as integers, text as bytes, and the loop reads
them straight out of the `PGresult` libpq produced -- nothing is copied between the driver and
the response. A create is one `INSERT ... RETURNING` in its own transaction, so the row is
committed when the statement returns and before the response is written (rule 8). A like's
`ON CONFLICT DO NOTHING` says through its row count whether it was the first (201) or a repeat
(200); the foreign key on `post_id` says 404.

**gzip** is done on the loop, before `h2o_send`, for bodies of 1 KB and more when the client
sent `Accept-Encoding: gzip`. Feeds shrink to about a quarter; a single post object would
shrink by a quarter and is sent as it is. Which library is a build-time choice (`build.zig`):

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
| h2o's own compress filter, zlib, glibc malloc as it comes | 76,000 | 3.26x | 3.2 |
| zlib q1, stream kept | 13,600 | 4.26x | 0.57 |
| libdeflate q1 | 9,100 | 4.38x | 0.38 |
| igzip L1 | 6,900 | 4.25x | 0.29 |
| igzip L0, table retrained from the previous response | 3,000 | 4.10x | 0.13 |

h2o's filter is not used: it opens a 270 KB deflate stream per response, which glibc hands back
to the kernel and takes again through page faults. That is the first row.

**Connections.** Each user holds a keep-alive connection, a file descriptor; the service gets
2 M of them. An idle socket costs about 3.9 KB in the kernel and 2 to 3 KB in h2o, no buffer,
so a million of them are under 8 GB. When `accept` fails with `EMFILE` the loop stops reading
its listeners for 100 ms instead of spinning; new users wait in the backlog.

**JWT.** HS256 only. The HMAC state after the key is derived once; per token what is left is
the hash over the token, the constant-time compare, and reading `sub`, `username` and `exp`.
Nothing about a token is remembered between requests.

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
src/server.c h2o: loops, two listeners per loop, routing, rendering, parked requests
src/db.c     the per-loop connection pools, pipelined, and the four statements
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
