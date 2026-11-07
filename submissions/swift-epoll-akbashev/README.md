# swift-epoll-akbashev

| | |
|---|---|
| Language | Swift 6.4.2 (official Ubuntu 24.04 tarball, sha256-checked), Swift 6 language mode |
| Framework | none: its own epoll event loops, HTTP/1.1 server and Postgres wire client |
| Driver | its own (SCRAM-SHA-256, extended protocol, binary parameters and results, pipelining) |
| Dependencies | none: the standard library, Synchronization and Glibc |
| Compression | gzip of its own (`Sources/Gzip`, RFC 1951/1952), for bodies of 1 KB or more when the request accepts it |
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
  transaction for all of them. A write goes to a write connection under a Sync of its own, so it commits alone
  and its response waits for the `ReadyForQuery` that confirms the commit (rule 8); a commit that fails answers
  500. A like on a missing post (`23503`) fails alone.
- **A pause after a light turn.** When a turn handled fewer than 32 events, the reactor sleeps 2 ms before its next
  `epoll_wait`, so what arrives meanwhile is served in one turn and its reads share one Sync; a busy turn never
  pauses, so the pause can't cap throughput. This is c-epoll-unorig-username's batching window (measured below).
  `PAUSE_US` (default 2000; 0 turns it off) and `PAUSE_BELOW_EVENTS` (default 32) set it. A busy turn's reads go 64
  to a Sync, then on to the next read connection, so one backend doesn't answer a whole turn alone.
- **Replies become responses as they are read.** A DataRow's columns are written as JSON straight out of the
  connection's receive buffer (binary `int8`, `int4` and `timestamptz`, truncated to milliseconds), and the
  response goes out with one `writev`.
- **The feed goes out gzipped.** Every request accepts gzip, and the box's network is part of the test, so a body of
  1 KB or more (in practice the feed, 5.3 KB to about 1.9) is compressed when the request accepts it; smaller ones
  (a post, an error) wouldn't shrink enough and some would grow. The compressor is Swift, with no library: one
  deflate block whose Huffman codes are built for the body, matches found greedily through a hash of the next four
  bytes (as the fastest levels of zlib and libdeflate find them), and an eight-bytes-a-step CRC-32. Its tables live
  as long as the reactor, so a response allocates nothing. About 14 µs per feed on an Apple M-series core.
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
  every request, then `sub` and `username` (SPEC.md's three 401 messages).

## Rules notes

- Queries: the five statements of SPEC.md exactly as written (plus `SELECT 1` for /health), one per request,
  prepared and pipelined. Nothing is cached across requests.
- Writes: each one commits in its own transaction before its response is sent.
- Connections: 64 to Postgres (`DB_CONNECTIONS`), well under 512.
- Session settings on its own connections only: `plan_cache_mode=force_generic_plan`.

## Measured locally

Docker on an Apple-silicon Mac (OrbStack, arm64): the server on 2 CPUs, Postgres 18 on 4 others (shared_buffers 1 GB),
`bench/mload` on the remaining 4; 20,000 users over both ports, `-gzip -think-scale 0.1` (~20,600 req/s), 30 s ramp,
60 s hold. CPU per request is each container's cgroup CPU time over 50 s of the hold, divided by the requests.
Runs alternated on a fresh copy of the seed each; numbers on this VM vary by ±20 % from run to run.

| | server µs/req | Postgres µs/req |
|---|---:|---:|
| this entry, no pause (`PAUSE_US=0`) | 24.9, 19.8 | 65.6, 50.0 |
| this entry | 9.5, 8.8, 10.7, 8.9 | 31.8, 29.2, 38.1, 30.0 |
| c-epoll-unorig-username (#1 at 16 cores), same runs | 11.7, 11.8 | 33.7, 32.0 |

c-epoll gzipped the feed in these runs (ISA-L level 0); this entry didn't yet.

## Running

```bash
sudo ./install.sh   # Swift 6.4.2 -> /opt/swift
./build.sh          # -> .build/release/Feed
DATABASE_URL=postgres://user:pass@host:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 ./start.sh
```

Optional: `IO_THREADS` (reactors; default: the CPUs in the affinity mask) and `DB_CONNECTIONS` (default 64).
