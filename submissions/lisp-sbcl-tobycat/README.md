# lisp-sbcl-tobycat: Common Lisp on SBCL, one epoll process per CPU

- Language: Common Lisp, compiled natively by SBCL 2.2.9 (Ubuntu 24.04's `sbcl` package)
- Framework / IO: none. epoll, sockets and `send`/`recv` through SBCL's FFI (`sb-alien`), plus hand-written HTTP/1.1
- Driver: hand-written Postgres wire protocol with SCRAM-SHA-256, prepared statements, pipelining and binary results
- Compression: hand-written gzip (greedy LZ77, one fixed-Huffman block, slicing-by-4 CRC-32)
- Crypto: hand-written SHA-256 and HMAC for HS256 and SCRAM
- Front: direct, on `PORT` and `EXTRA_PORT`
- Dependencies: SBCL and its bundled contribs (`sb-posix`, `sb-bsd-sockets` for name lookup, `sb-md5` for MD5 auth).
  No Quicklisp, no third-party libraries

## Design

- **Processes, not threads.** A supervisor forks one worker per CPU in the affinity mask (`WORKERS` overrides it).
  SBCL's garbage collector stops every thread in a process, so separate processes keep one worker's GC from pausing
  the others. Each worker has its own heap, its own `SO_REUSEPORT` listeners on both ports, its own epoll set and its
  own Postgres connections. Nothing is shared. Workers die with the supervisor (`PR_SET_PDEATHSIG`), and if any
  worker exits, the supervisor stops the rest.
- **One loop per worker:** level-triggered epoll on clients, listeners and Postgres sockets. A request is parsed in
  place from one 64 KB read buffer. Per-connection buffers exist only while a request or response is split across
  reads or writes, so an idle connection costs a small struct.
- **Postgres connections:** 64 in all (`max(2, 64 / workers)` per worker, `PG_CONNS` overrides it), a quarter of
  them for writes. All five statements are prepared at login, with typed parameters and binary results.
- **Reads:** every read in one pass over the epoll events goes to the read connection with the fewest outstanding
  requests. After the pass, each connection gets one Sync for its whole batch, and one `send`. Replies are matched
  in order, and each response is built straight from the `DataRow` bytes in the receive buffer.
- **Writes:** each insert gets its own Sync, so its own transaction. A like on a missing post fails alone (23503
  becomes 404), and every write response waits for its commit.
- **Responses:** the body is written after 256 bytes of reserved space, and the headers are filled in right to left
  in front of it, so a response is a single `send`. Bodies of 1 KB or more are gzipped when the client accepts it
  (`HTTP_GZIP=0` turns this off). A feed shrinks about 3.8×.
- **Validation:** strict RFC 8259 JSON parsing. The HS256 signature and a required `exp` are checked on every
  authenticated request. `alg` must be `HS256`. Whitespace is trimmed like JavaScript's `String.trim`, Unicode
  spaces included.
- **Hot paths** (SHA-256, gzip, JSON escaping, number and timestamp formatting) use fixnum and word arithmetic with
  type declarations, so SBCL compiles them to straight machine code. gzip reads its input through raw pointers:
  4-byte hash loads and 8-byte match compares.
- **HTTP:** keep-alive, pipelining (requests are answered in order), `Connection: close`, and HTTP/1.0.

## Losing Postgres

A lost connection fails its queued requests with 500, and the worker reconnects at the end of the same loop pass.
After a failed attempt it retries once a second, giving each connect and login step a 3 s limit. While a worker has
no usable connection, it answers 500, and `/health` returns the spec's 503 `degraded` body. If Postgres is
unreachable at startup, the server exits.

Killing all 64 backends under 4,000 req/s gave 322 errors (0.19% of the run), and then service was back to normal
with every connection restored.

## Rules notes

- The statements are exactly the ones in `SPEC.md` (plus `SELECT 1` for `/health`), one per request, prepared and
  pipelined.
- Nothing is cached across requests. The HMAC key's padded blocks are computed once at startup, which is a key
  schedule, not a cache of verifications.
- Session settings go in the startup packet, with the same effect as `SET`: `jit=off` and
  `max_parallel_workers_per_gather=0`.
- The server raises its soft open-file limit to the hard limit at startup.

## Measured

These are not official numbers. Everything ran on one 8-core machine: the server, Postgres 18 in Docker with
`max_connections=600`, and `mload`, all over loopback.

| Users | Think scale | Equivalent users | Hold | Req/s after ramp | p95 | p99 | Errors | Server CPU |
|---|---|---|---|---|---|---|---|---|
| 20,000 | 0.25 | 200,000 | 60 s | 8,450 | 1.1 ms | 1.8 ms | 0 | 0.70 cores |
| 50,000 | 0.125 | 1,000,000 | 90 s | ~22,400 (42,000 offered) | — | — | 0 | 2.1 cores |

The 50,000-user run didn't reach the offered rate. The whole box was saturated: Postgres used 3.9 cores, the server
2.1 and `mload` 1.2. Latency grew from queueing (fail). This shows the shared test machine's limit, not the server's.

A profile of one worker at 7,300 req/s puts 38% of samples in the kernel's `send` (loopback TCP, which also does
the receiver's work), 22% in gzip and CRC-32, and 7% in `recv`. JSON escaping, timestamps and parsing take a few
percent each.

Not yet tested: separate machines, real network, or anything near a million open connections.

## Running

```bash
sudo ./install.sh   # sbcl from apt
./build.sh          # compiles src/*.lisp and saves ./server (a standalone SBCL executable)
DATABASE_URL=postgres://user:pass@host:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 ./start.sh
```

`DATABASE_URL` may name the host by IPv4 address or name. User, password and database are percent-decoded.
Authentication can be SCRAM-SHA-256, MD5, password or trust.

## Files

| File | |
|---|---|
| `src/sys.lisp` | FFI syscalls, sockets, byte buffers |
| `src/crypto.lisp` | SHA-256, HMAC, PBKDF2, base64 |
| `src/json.lisp` | JSON parser and string escaping |
| `src/gzip.lisp` | DEFLATE (fixed Huffman) and CRC-32 |
| `src/pg.lisp` | Postgres startup, authentication, prepared statements, Bind/Execute |
| `src/server.lisp` | event loop, HTTP, routing, JWT, responses, supervisor |
| `build.lisp` | compiles everything and saves the executable |

## Credits

The architecture follows my `asm-epoll-tobycat` entry (per-core loops, one Sync per read batch, separate write
connections, in-place response building), which in turn follows `c-epoll-unorig-username`. The Lisp code was
written from scratch.
