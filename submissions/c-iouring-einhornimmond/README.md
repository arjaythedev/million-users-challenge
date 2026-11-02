# c-iouring-einhornimmond

C on an io_uring loop written on the system calls (`src/uring.h`, no liburing), PostgreSQL
through libpq in pipeline mode, HS256 over libsodium, requests parsed by picohttpparser, JSON
read by arnm 0.8.3 and written by additional code of the server, gzip through ISA-L's igzip or
libdeflate. Built with zig 0.15.2, which fetches and compiles the pinned dependencies
(picohttpparser, libsodium, arnm) from source; libpq, libdeflate and ISA-L come from apt.
Needs Linux 6.1 or later; Ubuntu 24.04's kernel 6.8.0-147 checks the reserved field of a
buffer-ring registration the wrong way round, which the server works around. The arrangement is
the one of gradido-unicorn's servers: one thread per loop, with the database driven from the
loops themselves.

This is the sibling of `c-h2o-einhornimmond` with the HTTP side on io_uring instead of h2o.
Shared with it, and described in [its README](../c-h2o-einhornimmond/README.md): one loop per
core with its own listeners under `SO_REUSEPORT` and its own share of the database
connections; the database driven from the loop, libpq in pipeline mode with one sync and one
commit per batch, a request answered only once the sync's result is in; the spec's statements
prepared once per connection with binary results; igzip at level 0 with a retrained Huffman
table; HS256 with nothing remembered between requests; the same rules kept the same way.

## What is different here

- **No h2o, no LibreSSL, no zlib.** The HTTP/1.1 server is 1,300 lines on io_uring; the
  request parser is picohttpparser, the one h2o uses. The build takes a minute instead of a
  few.
- **One call into the kernel per pass.** The loop enters the kernel once (`io_uring_enter`
  with `SINGLE_ISSUER` and `DEFER_TASKRUN`): that call hands over every send and every poll
  queued during the pass before and brings back every request, every completed send and every
  readable database socket since. Under load the loop lets completions collect for up to a
  millisecond first, so the cost of the call is shared by all of them. A request costs no
  `epoll_wait`, `read` or `sendmsg` of its own.
- **Accept and receive are multishot.** One accept stays armed per listener, into the ring's
  table of direct descriptors; one receive per connection, which borrows a buffer from a ring
  of 4,096 provided buffers of 2 KB when data arrives. A request is parsed where it arrived
  and the buffer goes back to the kernel when the handler returns. A response leaves in one
  piece, head and body in one send.
- **An idle connection holds no buffer**: 48 bytes of the server's memory and the kernel's
  socket, against 2 to 3 KB in h2o. A million users are 150 MB of resident memory here and
  1.8 GB there.
- **The database batches need no timer.** The pass itself collects the statements of a
  millisecond; the sync goes out at the end of every pass. The libpq sockets are watched with
  one-shot polls, armed again after each completion, because a multishot poll reports edges
  and would stay silent on bytes libpq left in the socket.
- **Less CPU, less memory.** On 8 and 4 cores a third less user time and a fifth less system
  time than h2o at the same load; on 16 cores the same. A twelfth of the resident memory.

## Results

The scoring runs of the challenge, as near as one box allows: a million users at the
challenge's profile (`-think-scale 2.5`, 42,000 requests a second), 7 minutes of ramp and a
5-minute hold, the server on 16, 8 and 4 cores of a Ryzen 5950X with one loop per core, the
load generator (`bench/cload`) on the other 16 threads, a local Postgres 15 with 64
connections. CPU over 200 s of the hold; the h2o rows are the same runs against
`c-h2o-einhornimmond`:

| cores | server | result | CPU | RSS |
|---:|---|---|---|---|
| 16 | io_uring | pass, p99 30 ms, max 413 ms, 0.009 % 503 | 1.8 cores (user 0.60, sys 1.18) | 431 MB |
| 16 | h2o | pass, p99 7.4 ms, max 157 ms, 0 errors | 1.8 cores (user 0.75, sys 1.09) | 1.9 GB |
| 8 | io_uring | pass, p99 9.0 ms, max 114 ms, 0 errors | 1.4 cores (user 0.48, sys 0.88) | 238 MB |
| 8 | h2o | pass, p99 8.3 ms, max 119 ms, 0 errors | 1.9 cores (user 0.79, sys 1.11) | 1.8 GB |
| 4 | io_uring | pass, p99 9.2 ms, max 83 ms, 0 errors | 1.1 cores (user 0.40, sys 0.72) | 150 MB |
| 4 | h2o | pass, p99 8.7 ms, max 112 ms, 0 errors | 1.5 cores (user 0.64, sys 0.87) | 1.8 GB |

The 503s of the 16-core run are one stall of the local Postgres (a checkpoint; 128 MB of
shared buffers, 1 GB of WAL between checkpoints) meeting 16 loops with 4 connections and a
queue of 512 requests each; the challenge's 512 connections give each loop eight times the
queue. The challenge allows 1 % errors, p99 under 1 s.

## Files

```text
install.sh   zig 0.15.2 (pinned, sha256-checked), libpq-dev, libdeflate-dev, libisal-dev
build.sh     zig build --fetch; zig build -Doptimize=ReleaseFast
start.sh     bin/server, DB_POOL_SIZE defaulting to 512
src/main.c   environment, RLIMIT_NOFILE
src/uring.h  the ring on the system calls: setup, enter, direct descriptors, provided buffers
src/server.c the loops, two listeners per loop, routing, rendering, parked requests
src/db.c     the per-loop connection pools, pipelined, and the four statements
src/gz.c     igzip or libdeflate behind one function
src/jwt.c    HS256 verification
bench/       cload, the challenge's load generator in C (see bench/README.md)
```

`SERVER_THREADS` (default: one loop per core) and `DB_POOL_SIZE` are the two knobs.

## License

MIT, under the [license](../../LICENSE) of the repository.
