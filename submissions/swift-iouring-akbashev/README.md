# swift-iouring-akbashev

swift-epoll-akbashev on io_uring: the same reactors, HTTP, Postgres client and gzip, with each reactor's waiting,
accepting and receiving moved from epoll onto a ring of its own.

| | |
|---|---|
| Language | Swift 6.4.2 (official Ubuntu 24.04 tarball, sha256-checked), Swift 6 language mode |
| Framework | none: its own io_uring event loops (multishot accept and receive, provided buffers), HTTP/1.1 server and Postgres wire client |
| Driver | its own (SCRAM-SHA-256, extended protocol, binary parameters and results, pipelining) |
| Dependencies | none: the standard library, Synchronization and Glibc; io_uring is Swift over its system calls (`Sources/IORing`, swift-underlord's `IORing`) |
| Compression | gzip of its own (`Sources/Gzip`, RFC 1951/1952, ISA-L's default Huffman table), for bodies of 1 KB or more when the request accepts it |
| Postgres connections | 64 in all: per reactor, three quarters of its share for reads and a quarter for writes, at least one each |

## Design

- **One reactor per core, sharing nothing.** A reactor is an actor whose executor is its own event loop: a thread and
  an eventfd, conforming to `SerialExecutor` and `TaskExecutor`; the reactor waits in its own ring's `io_uring_enter`,
  with a read of the eventfd armed, so a job queued for it wakes it. The server is one task group
  with a child task per reactor. Each reactor owns its listeners (an `SO_REUSEPORT` socket on each of the two ports),
  its client connections, a file descriptor table of its own (`unshare(CLONE_FILES)`) and its Postgres connections.
  Nothing crosses between threads on the way from a request to its response, and nothing is `@unchecked Sendable`.
- **A ring per reactor.** Made on the reactor's own thread (`SINGLE_ISSUER`, `DEFER_TASKRUN`, `COOP_TASKRUN`): a
  multishot accept on each listener, and one multishot receive per connection into the ring's provided buffers
  (8,192 of 2 KB per reactor): a buffer is taken only when bytes arrive, the request is parsed in place and the buffer
  goes back at the end of the turn, so an idle connection holds none. Receives that found no buffer are armed again
  once the turn's buffers are back. Responses still go out with one `writev` each; a socket that doesn't take a whole
  response gets a one-shot poll for the rest. A connection is shut down before it is closed, so its armed receive
  completes; completions carry the slot's epoch, so late ones for a closed connection are told apart. The buffer ring
  is registered the way Ubuntu's 6.8.0-136 and later kernels accept (an inverted check on its reserved words).
- **Postgres on the same ring.** Each Postgres connection logs in and prepares SPEC.md's statements on a blocking
  socket, then turns non-blocking and gets a multishot poll on its reactor's ring. A turn's reads go to the read
  connection with the fewest requests waiting, under one Sync: one write, one round trip and one implicit
  transaction for all of them. A turn's writes go the same way to a write connection, under a Sync of their own:
  one implicit transaction and one commit for all of them (group commit), and each response waits for the
  `ReadyForQuery` that confirms it (rule 8). If one of them fails, Postgres rolls the group back: a like on a
  missing post (`23503`) answers 404, and the others run again, each alone under a Sync of its own; a write whose
  commit fails alone answers 500.
- **A fixed cadence.** A turn that didn't take 1,024 completions lasts 2 ms from its wait, the
  rest slept, so what arrives meanwhile is served in the next turn: its reads share one Sync and its writes one
  commit. A full turn goes straight on, so the cadence can't cap throughput. This is c-epoll-unorig-username's
  batching window (measured below). `PAUSE_US` (default 2000; 0 turns it off) sets it. A busy turn's reads go 64 to
  a Sync, then on to the next read connection, and so do its writes, so one backend doesn't answer a whole turn alone.
- **Linear retransmission timeouts.** Postgres sockets and the listeners set `TCP_THIN_LINEAR_TIMEOUTS` (accepted
  sockets inherit it, and `TCP_NODELAY`, from their listener): with the few packets a request and its response have
  in flight, a segment lost twice costs two retransmission timeouts (400 ms) instead of three (600 ms).
- **A new connection is accepted with its request.** The listeners set `TCP_DEFER_ACCEPT` (1 s), so accept sees a
  connection once its first bytes have arrived, and the reactor reads it right away: the first request is served in
  the turn that accepts it, before its multishot receive is armed.
- **Replies become responses as they are read.** A DataRow's columns are written as JSON straight out of the
  connection's receive buffer (ids and like counts as text, their digits copied as they are; `timestamptz` binary,
  truncated to milliseconds), and the
  response goes out with one `writev`.
- **The feed goes out gzipped.** Every request accepts gzip, and the box's network is part of the test, so a body of
  1 KB or more (in practice the feed: about 3.7 KB to 1.15 under the load's short posts) is compressed when the
  request accepts it; smaller ones (a post, an error) wouldn't shrink enough and some would grow. The compressor is
  Swift, with no library. Its one deflate block is coded with ISA-L's default Huffman table, the one ISA-L's fastest
  level uses (its block header, taken from ISA-L 2.31.1 under Intel's BSD license, which `Sources/Gzip/Gzip.swift`
  carries), so a body takes one pass, each literal or match written as it is found. The loop is ISA-L's level-0
  assembly loop, in Swift: two positions per step, each with 8 bytes compared against its candidate at once (a match
  of 4 to 7 bytes needs no loop), a 16-bit hash table, a literal and a match or two literals in one write, and a
  64-bit bit writer with no branch. The CRC-32 runs on the CPU's carry-less multiply, 256 bytes per step with AVX-512's
  VPCLMULQDQ (`Sources/CRCExtensions512`) or 128 with PCLMULQDQ (`Sources/CRCExtensions`), through Swift's `Builtin`
  and chosen by CPUID with a check against its tables at start-up, as the SHA module is; eight bytes per step from
  tables otherwise. The start-up line says which. Its tables live as long as the reactor, so a response allocates
  nothing. On an AMD EPYC 9V74 runner core, 3.3 µs per feed against 3.4 for ISA-L's level 0 (14 before), with output
  the same size.
- **Idle connections cost almost nothing.** Requests are parsed in place out of the buffer their receive filled; a
  connection keeps a buffer only while a request or response is split across reads or writes. A slot is 40 bytes.
- **Ownership.** Buffers, connection slots and Postgres connections are `~Copyable`; the HTTP request and the JSON
  reader are `~Escapable` views with `@_lifetime`; bytes are read through `Span` and `RawSpan` and written through
  `OutputSpan` and `MutableSpan`; SHA-256's state and response heads are `InlineArray`s.
- **The JSON reader builds nothing.** Request bodies and token claims are checked whole and only the members the
  caller names are kept, as places in the bytes; strings are read in place and copied only when they must outlive
  the request (a new post's text, the token's username, which the response echoes). Error answers are static
  strings, written into one reused buffer.
- **Half-closed and pipelined requests.** A client that half-closes gets the answers to what it sent before the
  connection closes; pipelined requests are answered in order, one at a time.
- **Auth.** HS256 with the secret's ipad/opad midstates computed once; signature, `alg` and a required `exp` on
  every request, then `sub` and `username` (SPEC.md's three 401 messages). SHA-256 runs on the CPU's SHA extensions
  where it has them: Swift has no counterpart to C's x86 intrinsics, but its compiler's `Builtin` module reaches the
  instructions (`Sources/SHAExtensions`, built with the compiler's hidden `-enable-builtin-module` flag and the SHA
  instructions enabled for that module alone). At start-up the server asks CPUID, through glibc's x86 feature table
  (`<sys/platform/x86.h>`), whether they are usable, and checks that the module hashes test messages as its portable
  Swift function does; otherwise it uses the portable one.
- **JSON strings are copied as they are checked**, 16 bytes at a time (`SIMD16`), then 8, then one.

## Rules notes

- Queries: the five statements of SPEC.md exactly as written (plus `SELECT 1` for /health), one per request,
  prepared and pipelined. Nothing is cached across requests.
- Writes: a turn's writes on a connection commit together in one transaction (group commit, rule 8), and each
  response is sent after that commit.
- Connections: 64 to Postgres (`DB_CONNECTIONS`), well under 512.
- Session settings on its own connections only: `plan_cache_mode=force_generic_plan`.

## Measured locally

Docker on an Apple-silicon Mac (OrbStack, arm64): the server on 2 CPUs (2 reactors), Postgres 18 on 4 others,
`bench/mload` on the remaining 4; 200,000 users over 8 loopback addresses and both ports, `-gzip -think-scale 2.5`
(~8,400 req/s), 60 s ramp, 2 min hold, each run on a fresh copy of the seed, alternating with swift-epoll-akbashev.
CPU per request is the server's user and system time over 90 s of the hold, divided by the requests.

| | server µs/req | p95 | p99 |
|---|---:|---:|---:|
| this entry | 19.2, 33.4 | 41, 277 ms | 489, 933 ms |
| swift-epoll-akbashev | 19.3, 32.2 | 24, 345 ms | 322, 1,031 ms |

The VM has slow phases (about 33 µs per request and stalls of a second, in runs of both), so on this rig the two
are at parity: moving waits, accepts and receives onto the ring didn't change CPU per request measurably, with
responses still written by `writev`.

## Running

```bash
sudo ./install.sh   # Swift 6.4.2 -> /opt/swift
./build.sh          # -> .build/release/Feed
DATABASE_URL=postgres://user:pass@host:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 ./start.sh
```

Optional: `IO_THREADS` (reactors; default: the CPUs in the affinity mask), `DB_CONNECTIONS` (default 64),
`DEFER_ACCEPT` (`TCP_DEFER_ACCEPT` seconds on the listeners; default 1, 0 turns it off), `RING_BUFFERS` (provided
buffers per reactor, a power of two; default 8192) and `RING_BUFFER_SIZE` (bytes each; default 2048). Linux 6.0 or
later (multishot receive).
