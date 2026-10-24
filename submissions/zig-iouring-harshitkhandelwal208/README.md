# Zig: io_uring with direct descriptors, own HTTP, own Postgres client, own gzip

No framework, no libc, no driver, no compression library: raw `io_uring` syscalls, a hand-written HTTP/1.1 server, a
hand-written PostgreSQL protocol client (SCRAM-SHA-256 and MD5, extended protocol, binary parameters and results,
pipelined), a streaming JSON writer, HS256 JWTs, and a gzip encoder (fixed-Huffman deflate whose main loop is
hand-written x86-64 assembly, plus a PCLMULQDQ CRC-32), all on the Zig standard library.

## Stack

| Piece | Version |
|---|---|
| Zig | 0.17.0 (official tarball, SHA-256 pinned in `install.sh`) |
| I/O | `io_uring`: single issuer, deferred task work, **direct (fixed) descriptors**, multishot accept, multishot recv on a provided-buffer ring |
| HTTP | own HTTP/1.1: keep-alive, pipelining, `Content-Length` and chunked bodies, half-close |
| Postgres | own protocol-3.0 client, no `libpq`; PostgreSQL 18 on the other box |
| JSON | own writer (vectorised string escape) and strict reader for request bodies |
| JWT | HS256 with `std.crypto` HMAC-SHA256; the key pads are derived once, every request still verifies signature, `alg` and `exp` |
| gzip | own encoder: `src/deflate.zig` (algorithm, Zig reference loop), `src/deflate_core.zig` (assembly loop), CRC-32 with `pclmulqdq` (`src/crc32.zig`) |
| Dependencies | none (`install.sh` only unpacks the pinned Zig toolchain) |

`build.sh` runs `zig build -Doptimize=ReleaseFast -Dcpu=native` (built on the box, so SHA-NI / PCLMUL / AVX2 paths follow
the CPU); the binary is about 530 KB.

## How it uses the cores

- `WORKERS` threads, two by default on any number of cores: fewer, busier workers batch more completions per system call
  (measured on the same four logical CPUs: 15 µs of server CPU per request with two workers, 17 to 19 with four), and the
  cores the workers do not use are left to the kernel's network processing. Each worker owns its `io_uring`, **two
  `SO_REUSEPORT` listeners (`PORT` and `EXTRA_PORT`)**, its connection table and its Postgres connections; nothing is
  shared on the request path, so there is no lock to contend on and no cross-core hand-off. One process, so `LimitNOFILE`
  is not a constraint: client sockets are never process file descriptors.
- **Direct descriptors.** Accepted sockets go straight into the ring's fixed-file table (`accept_direct`, auto-allocated
  slot), and every `recv`, `send`, `shutdown` and `close` addresses the slot with `IOSQE_FIXED_FILE`: no `fd` allocation
  in the process, no per-operation `fget`/`fput`, and the slot index doubles as the connection id. Each ring gets
  `SLOTS` entries (default 2.1 M / workers, at most 2^20 and at most `RLIMIT_NOFILE`).
- **An idle keep-alive connection costs a 16-byte record** in an `mmap`ed table plus its kernel socket and its armed
  multishot recv. Request bytes arrive in buffers picked by the kernel from a shared provided-buffer ring and are
  returned the moment they are parsed; a connection only gets an extension (pending bytes or a response in flight)
  while it needs one. 192,000 idle connections held on a laptop: the whole server was 19 MB of resident memory.
- **No timers per connection.** Idle connections are never touched (no TCP keep-alive probes, no per-connection
  timeouts); the one-second tick only revisits connections that hold an incomplete request (30 s limit).
- **Completion batching.** A loop that has just handled a batch waits (`io_uring_enter` with `IORING_ENTER_EXT_ARG`) for up
  to `COALESCE_US` (1 ms) for `COALESCE_MIN` (32) completions before waking, so under load one system call serves many
  requests and their Postgres traffic leaves in a few large writes; an idle loop wakes on the first event. The delay is
  far below the latency budget; `COALESCE_US=0` turns it off.

## Postgres

- **One statement per request**, exactly the SQL of SPEC.md, prepared once per connection, binary parameters and results
  (Postgres never formats a timestamp or integer). Session settings only: `plan_cache_mode=force_generic_plan`, `jit=off`,
  `max_parallel_workers_per_gather=0`.
- **Reads are pipelined and packed.** `DB_READ_CONNS` (64 in total, spread over the workers) connections each carry up
  to `PG_DEPTH` (32) requests in flight, so throughput is bounded by what Postgres executes, not by the network round
  trip. Consecutive reads go to the same connection (`PG_PACK`, 32, before the least loaded one is chosen again), and the
  reads a connection receives in one pass of the event loop leave as **one pack: their Bind/Execute pairs and a single
  `Sync`**. A pool sends every query on its own (a `send`, a backend wake-up and a `recv` each), and Postgres flushes its
  output at every `Sync`, so every reply is at least one packet; a pack is one `send`, one wake-up, and replies that
  leave in as few full packets as their bytes need. Each reply is still delivered as soon as its CommandComplete arrives,
  so no request waits for the rest of its pack. If a statement in a pack fails, Postgres skips the rest until the `Sync`:
  the failing request gets its error and the skipped reads are sent again (`TEST_FAIL_PACKS=1` makes the first attempt of
  every pack fail, to test exactly that). Under READ COMMITTED every statement still takes its own snapshot, so a read in
  a pack sees exactly what it would see on its own.
- **Writes are committed in groups.** `DB_WRITE_CONNS` (16 in total) connections each run one transaction at a time;
  writes that arrive while a transaction is committing are staged and then sent together as a single transaction
  (one `Sync`, one WAL flush for all of them). Every response is sent after the commit that contains its write. A group
  that fails (for example a like on a post that does not exist, a foreign-key violation) is rolled back as a whole and
  each member is then retried on its own, so one bad request cannot fail its neighbours.
- **No cache of any kind** survives a request: no response, query-result, table or token cache. Only connections,
  prepared statements and the HMAC key pads are kept.
- **A database that goes silent** (a crashed or partitioned host sends no reset): a connection with requests in flight and
  no reply for 10 s is declared lost; the others are then put on a 3 s deadline and each idle one is given a bare `Sync` to
  answer, so the whole pool is found out within seconds instead of one request at a time. Postgres sockets also carry TCP
  keepalive (probe after 20 s idle) and `TCP_USER_TIMEOUT` (15 s). `/health` answers 503 `degraded` meanwhile, and everything
  recovers when the database is back (`tests/pgstall.py` freezes a proxy in front of Postgres to check this). A FATAL error
  from a terminating backend is treated as a lost connection, not as the failure of one query: the reads of its pack are sent
  again. All timeouts run on the monotonic clock, so a clock step cannot expire or extend them.
- **Connection loss.** A dead Postgres connection is replaced on a helper thread (handshake timeouts, so a stalled server
  cannot freeze the event loop). Reads in flight on it are idempotent and are sent again on another connection; writes in
  flight get a 500 (one may have committed); writes not yet sent, and reads with no connection up, wait up to 3 s for a
  reconnect. The server starts even if Postgres is not reachable yet: `/health` answers 503 `degraded` until it is. The
  database host may be an IPv4 address or a name (`/etc/hosts`, then the nameservers of `/etc/resolv.conf`).

## gzip

`Accept-Encoding: gzip` is sent on every request of the benchmark and the feed is several KB of JSON, so it is compressed;
the single-post and error bodies (a few hundred bytes) are not.

- One pass, no tokens in between: greedy LZ77 over a two-way hash table, every literal and match written straight to a
  64-bit bit accumulator in fixed-Huffman form. No histogram, no tree, no second pass, no allocation; the output is
  written straight into the response buffer.
- **The main loop is assembly** (`deflate_core.zig`, selected when the CPU has BMI1/BMI2 and the input is at most 32 KiB;
  otherwise the Zig loop in `deflate.zig` runs). The compiled loop kept its state in stack slots, re-checked conditions
  that cannot fail for such inputs (an entry at least `base` is always an earlier position of this call, and its distance is
  below 32768), and branched on a flush after every symbol. The assembly keeps everything in registers, uses `shlx`,
  `bzhi`, `tzcnt` and one flush per symbol, and indexes match interiors with a tight loop: 26% fewer instructions and
  13 to 26% less time depending on input. The two loops produce **identical bytes**: a unit test compares them on 4,000
  random inputs of seven shapes (including sizes around the 32 KiB limit), and all 4,980 feeds of two corpora (seed-shaped
  and load-test-shaped) were encoded with both, compared, and decompressed with Python's `gzip` and `zlib`.
- CRC-32 with carry-less multiplication (`pclmulqdq`, 64 bytes per fold step) in inline assembly, with a table fallback for
  the tail.
- Sizes and times (one core, Ryzen 7 7445HS): the feed the load test produces (20 posts of the form "name says hi at
  time (VU n, iter k)", 3.6 KB) becomes 985 bytes, a single TCP segment, in 3.5 µs when the same feed is requested again
  and again (as it is in a run: one new post per ~50 reads) and 6.2 µs when every feed is new; the 5.1 KB seed feed becomes
  2,359 bytes in 7.4 µs. (A dynamic-Huffman encoder gets the seed feed to about 1,970 bytes but needs a second pass.)
- Experiments that were slower and are not used: a branch-free loop (24 µs against 9.5, because it serialises each symbol
  on the one before), one merged candidate test (13 µs), an unconditional 64-bit flush in the compiled loop (10 µs).
- `tests/gzip_check.py` round-trips 21 shapes through Python's `gzip` and `zlib`, and `tests/httptest.py` checks that the
  compressed feed decompresses to exactly the plain feed, including feeds whose posts are full of characters that need
  escaping.

## Profile-driven changes

The server was profiled with `perf` under load (unstripped build, instruction level), and the program's own code was
fixed in order of what it cost. Kernel networking is about 70% of the samples, program code about 20%.

| finding in the profile | change | effect |
|---|---|---|
| gzip: 61% of the program's time, in a compiled loop full of spills | assembly main loop (above) | -23% on the live feed shape |
| `jw.putTimestamp`: 2.6% of all samples (20 per feed, signed divisions) | unsigned arithmetic, two-digit tables, the date of the previous timestamp reused | 2.6% to 0.5% |
| `parseLoop`: half of it in a byte-by-byte scan for `\r\n\r\n` (the library lowers a multi-byte search to a byte loop) | one pass over the request in 32-byte blocks that finds the end of the head and records every line end; the first space and the header colons by block compare | 2.3% to 1.8%, and a flat profile |
| `memcpy` of the compressed body into the response buffer | compress straight into the response buffer | -0.3% |
| `Content-Length` formatted with `std.fmt` on every response | digits written directly | small |
| string escaping: scalar tail loops | 32-byte block compare, block store when clean, escape position from the mask | 1.4% (was already vectorised) |

Everything that remains in the program is under 2% per function except the gzip loop (about 10%).

## Configuration

`DATABASE_URL`, `JWT_SECRET`, `HOST`, `PORT`, `EXTRA_PORT` as in SPEC.md. Optional: `WORKERS`, `DB_READ_CONNS`,
`DB_WRITE_CONNS` (the sum over all workers stays under 512), `PG_DEPTH`, `PG_PACK`, `SLOTS`, `COALESCE_US`,
`COALESCE_MIN`, `STATS=1` (a line per worker every second). Test hooks, off by default: `TEST_FAIL_GROUPS=1` poisons every
multi-member write group, `TEST_FAIL_PACKS=1` poisons the first attempt of every read pack, so the retry paths run.

## Tests

- `zig build test`: unit tests (CRC-32 against `std`; JSON escape, integers and timestamps against byte-wise and
  general references on random input; JWT claims; HTTP framing, and the head scanner against a plain search on 100,000
  random heads; JavaScript-style trimming; id parsing; the assembly and Zig encoder loops against each other).
- `bash test/run.sh submissions/zig-iouring-harshitkhandelwal208`: the 42-check suite of the challenge, on port 3000.
- `tests/httptest.py`: raw-socket HTTP and gzip: pipelining, byte-at-a-time requests, split and chunked bodies,
  `Connection: close`, HTTP/1.0, half-close, malformed and oversized heads, conflicting lengths, JavaScript-style
  trimming, astral-plane characters at the 500-character limit, escapes in bodies and token usernames, every
  `Accept-Encoding` variant, odd paths around `/posts/…/like`, chunked framing abuse (endless trailers, huge extensions,
  more than 1024 chunks), a 60,000-request pipelined burst on one connection, and both ports.
- `tests/pgstall.py`: Postgres goes silent behind a freezable proxy: `/health` reports 503 within about 15 s, and service
  resumes when the proxy is unfrozen.
- `tests/writetest.py`: 300 users post and like at once (plus 40 concurrent duplicate likes, and likes of a missing post in
  the middle of the groups); every response and the final counts are checked. Both suites are also run with
  `TEST_FAIL_GROUPS=1 TEST_FAIL_PACKS=1 DB_WRITE_CONNS=1`, which makes every write group and every first read pack fail.
- Also run: a ReleaseSafe build (bounds and overflow checks) through the same suites; Postgres unreachable at start and
  then reachable; every Postgres backend killed during a load run (ReleaseSafe build; see below).

## Local results (not the official boxes)

Ryzen 7 7445HS laptop, Postgres 18.6 built from source and pinned to two cores, the server on two other cores, `mload`
on four more, all over loopback, so these numbers show what the server costs, not what the benchmark box will do. CPU per
request is the server's user + system time divided by the requests it served. One property of loopback matters for
reading them: a packet's receive processing runs in the context of whoever *sent* it. The server pays for delivering its
responses into the client's socket (work a real network card would not charge it), and does not pay for receiving the
client's requests or Postgres's replies (work that on a real network lands on the app box's cores).

| 20,000 users at 0.1 think scale (about 20,000 req/s, 5% writes), 5 minutes | result |
|---|---|
| requests / errors | 6.55 M / 0 |
| server CPU per request (user + system) | 13.4 µs (0.27 cores busy, 2 workers; 12.7 to 14 µs across runs) |
| p50 / p95 / p99 | 2.4 ms / 4.2 ms / 9.9 ms (Postgres had two cores) |
| server resident memory | 21 to 22 MB throughout |

At 45,000 req/s (30,000 users, 0.05 think scale) the two server threads used 0.70 cores in total (15.5 µs per request);
there the latency (p95 about 200 ms) was Postgres saturating its two cores, not the server.

Where the CPU goes (`perf`, 19,000 req/s): about 70% kernel (TCP and `io_uring`; this laptop also runs a firewall that is
another 5% and should not exist on the benchmark box), about 20% the program, of which about 10 points are the gzip
loop and under 2 points each everything else. User time is about 3 µs per request.

Packets: TCP segments per request, counted for every party, went from 4.9 to 4.1 with packs. Three of them are the
request, the response and the client's ACK (the server's ACK of a request rides on its response), so the
Postgres traffic per request fell from about 1.9 segments to about 1.1. In the same back-to-back runs the server's own CPU
moved only from 14.1 to 13.9 µs, because on loopback the receive side of those packets is not charged to it.

What the Postgres scheduling bought (server CPU per request, two cores, 17,700 req/s, measured in one series):

| change | µs per request |
|---|---|
| one request per Postgres connection (pool-style), no batching | 32.5 |
| + 200 µs completion batching, `PG_PACK=8` | 22.8 |
| `PG_PACK=16`, `COALESCE_US=1000` | 17.2 |
| `PG_PACK=32`, `COALESCE_US=1000` (default) | 16.5 |

A measurement build that answered reads without Postgres cost 17 µs per request in the same series, against 31 µs with the
pool-style policy, so Postgres traffic is no longer a visible part of the server's cost.

Idle connections: 192,000 keep-alive connections (each had made one request) held at once: the server's resident memory
was 19 MB, against 6.5 MB before the first client connected; a probe request during the hold took 0.5 ms.

Postgres failure (ReleaseSafe build, 20,000 users at 0.25 think scale, about 520,000 requests per run): killing all 80
backends twice in a run cost 70 to 74 failed requests (0.014%); reads in flight are sent again, writes that may already
have committed are not. The server reconnected within a second each time and recovered without intervention.

### Kernel time: what was tried and found not to help

Kernel TCP and `io_uring` are about 70% of the server's samples, so this was measured carefully. Kernel *instructions per
request* (`perf stat`, 25 s of steady state) are stable to 0.3% between identical runs, which time and cycles are not
(5 to 10%), so options were compared on that, interleaved. At about 18,400 req/s the baseline is 33.8k kernel
instructions and about 57.7k kernel cycles per request (an IPC of 0.59: the kernel's TCP code is bound by cache misses on
per-connection state, not by instruction count).

| tried | kernel instructions per request |
|---|---|
| baseline | 33,800 to 33,950 |
| congestion control `reno` instead of `cubic` | 33,780 / 33,845 |
| `SO_RCVBUF` or `SO_SNDBUF` fixed at 64 KB (no autotuning) | 33,976 / 33,861 |
| Nagle on (`TCP_NODELAY` off) | 33,863 |
| completion window 2 ms, pack 64 | 32,371 |
| completion window 4 ms, pack 64 to 100 | 31,260 to 31,750 |
| completion window 16 ms, pack 100 | 29,291 |

Socket options change nothing (all within 0.4%). A longer window removes up to 13% of the instructions but not the
cycles (57.7k against 57.6k kernel cycles per request at 4 ms) because the saved instructions were the cheap, cache-warm
ones; it lowers user cycles by about 8% and raises p95 from 4 ms to 16 ms (73 ms at 16 ms), so the default stays at 1 ms.
The remaining kernel work is the TCP transmit and receive path of one response, one request and one ACK per request, plus
the Postgres traffic, none of which user space can shorten without breaking the protocol. About 13% of the samples on
this laptop are netfilter hooks that the benchmark box (no firewall rules loaded) should not have.

## Rules

No cache across requests (rule 7); every write is committed before its response (rule 8); signature, algorithm and `exp`
of every token are checked on every request that carries one (rule 9). Postgres is only configured through session
settings; no schema, index, function or extension is touched (rules 3, 4). At most 512 Postgres connections (default about
80 in total). Nothing detects the load test.

## Limits

The assembly gzip loop needs BMI1 and BMI2 (any x86-64 CPU since 2013/2015); without them the Zig loop produces the same
bytes more slowly. Needs Linux 6.0 or newer with `io_uring` enabled (multishot recv with provided buffers; Ubuntu 24.04's 6.8 qualifies);
there is no epoll fallback. IPv4 only. `Expect: 100-continue` is not answered (a client that waits for it sends the body
after its own timeout). Bodies over 8 MB and heads over 16 KB are refused; chunked bodies may have at most 1024 chunks,
size lines of at most 64 bytes and 4 KiB of trailers.
