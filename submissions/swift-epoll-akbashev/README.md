# swift-epoll-akbashev

| | |
|---|---|
| Language | Swift 6.4.2 (official Ubuntu 24.04 tarball, sha256-checked), Swift 6 language mode |
| Framework | none: its own epoll event loops, HTTP/1.1 server and Postgres wire client |
| Driver | its own (SCRAM-SHA-256, extended protocol, binary parameters and results, pipelining) |
| Dependencies | none: the standard library, Synchronization and Glibc |
| Compression | gzip of its own (`Sources/Gzip`, RFC 1951/1952, ISA-L's default Huffman table), for bodies of 1 KB or more when the request accepts it |
| Postgres connections | 64 in all: per reactor, three quarters of its share for reads and a quarter for writes, at least one each |

## Design

- **One reactor per core, sharing nothing.** A reactor is an actor whose executor is its own event loop: a thread, an
  epoll instance and an eventfd, conforming to `SerialExecutor` and `TaskExecutor`. The server is one task group
  with a child task per reactor. Each reactor owns its listeners (an `SO_REUSEPORT` socket on each of the two ports),
  its client connections, a file descriptor table of its own (`unshare(CLONE_FILES)`) and its Postgres connections.
  Nothing crosses between threads on the way from a request to its response, and nothing is `@unchecked Sendable`.
- **Postgres in the same epoll set.** Each Postgres connection logs in and prepares SPEC.md's statements on a
  blocking socket, then turns non-blocking and joins its reactor's epoll set. A turn's reads go to the read
  connection with the fewest requests waiting, under one Sync: one write, one round trip and one implicit
  transaction for all of them. A turn's writes go the same way to a write connection, under a Sync of their own:
  one implicit transaction and one commit for all of them (group commit), and each response waits for the
  `ReadyForQuery` that confirms it (rule 8). If one of them fails, Postgres rolls the group back: a like on a
  missing post (`23503`) answers 404, and the others run again, each alone under a Sync of its own; a write whose
  commit fails alone answers 500.
- **A fixed cadence.** A turn that didn't fill the event array (1,024 events) lasts 2 ms from its `epoll_wait`, the
  rest slept, so what arrives meanwhile is served in the next turn: its reads share one Sync and its writes one
  commit. A full turn goes straight on, so the cadence can't cap throughput. This is c-epoll-unorig-username's
  batching window (measured below). `PAUSE_US` (default 2000; 0 turns it off) sets it. A busy turn's reads go 64 to
  a Sync, then on to the next read connection, and so do its writes, so one backend doesn't answer a whole turn alone.
- **Linear retransmission timeouts.** Postgres sockets and the listeners set `TCP_THIN_LINEAR_TIMEOUTS` (accepted
  sockets inherit it, and `TCP_NODELAY`, from their listener): with the few packets a request and its response have
  in flight, a segment lost twice costs two retransmission timeouts (400 ms) instead of three (600 ms).
- **Replies become responses as they are read.** A DataRow's columns are written as JSON straight out of the
  connection's receive buffer (binary `int8`, `int4` and `timestamptz`, truncated to milliseconds), and the
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
- **Idle connections cost almost nothing.** Requests are parsed in place out of the reactor's read buffer; a
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

Docker on an Apple-silicon Mac (OrbStack, arm64): the server on 2 CPUs, Postgres 18 on 4 others (shared_buffers 1 GB),
`bench/mload` on the remaining 4; 20,000 users over both ports, `-gzip -think-scale 0.1` (~21,000 req/s), 30 s ramp,
60 s hold. CPU per request is each container's cgroup CPU time over 45 s of the hold, divided by the requests; TCP
segments are the server's and Postgres's `/proc/net/snmp` counters over the same window (the network's MTU is
1,500). Runs alternated on a fresh copy of the seed each; CPU on this VM varies by ±20 % from run to run.

| | server µs/req | Postgres µs/req | TCP segments/req (to Postgres) | p99 |
|---|---:|---:|---:|---:|
| this entry | 11.0, 11.7 | 25.5, 25.7 | 3.29, 3.36 (0.10, 0.10) | 9.4, 9.9 ms |
| before group commit and the fixed cadence | 15.6, 11.6 | 39.4, 27.9 | 3.52, 3.46 (0.18, 0.18) | 59.9, 40.2 ms |
| c-epoll-unorig-username, same runs | 9.1 | 23.8 | 3.28 (0.10) | 8.9 ms |

With one reactor taking all of it (`IO_THREADS=1`), the fixed cadence against the old pause after light turns:
Postgres 21.6 and 22.3 µs/req against 27.4 and 31.1, p95 7.6 and 8.4 ms against 11.8 and 12.1.

## Running

```bash
sudo ./install.sh   # Swift 6.4.2 -> /opt/swift
./build.sh          # -> .build/release/Feed
DATABASE_URL=postgres://user:pass@host:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 ./start.sh
```

Optional: `IO_THREADS` (reactors; default: the CPUs in the affinity mask) and `DB_CONNECTIONS` (default 64).
