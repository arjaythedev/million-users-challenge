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
- **Reads:** the reads of one pass over the epoll events share one read connection (the one with the fewest
  outstanding requests), up to 64 per batch. After the pass, each batch gets one Sync, and so one round trip and one
  `send`. Replies are matched in order, and each response is built straight from the `DataRow` bytes in the receive
  buffer.
- **Batching window:** after a pass of fewer than 32 events, the worker sleeps 2 ms (`BATCH_US`, 0 turns it off), so
  the next pass has more reads to batch. Together with one connection per pass, this cut Postgres's transactions at
  42,000 req/s from about 24,700/s to 5,200/s, and its CPU from about 10 cores to 3.6.
- **Writes:** the writes of one pass share one write connection and one Sync, so one transaction and one commit
  (group commit), up to 64 per batch. A write is answered only at the ReadyForQuery after its Sync, when its
  transaction has committed, not at its CommandComplete. If a statement in the batch fails, Postgres rolls the whole
  transaction back: a like on a missing post (23503) gets its 404 alone, and the other writes of the batch, which
  committed nothing, are each sent again in a transaction of their own. Any other error in a shared transaction (a
  deadlock between two batches, say) resends that write the same way. At 84,000 offered req/s this cut write p95
  from about 7 s to 120 ms and Postgres CPU from 8.8 to 6.7 cores. Posts created in the same batch share their
  `created_at` (`now()` is the transaction's start); the feed's `id DESC` tiebreak still orders them newest first.
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
- Writes are batched into one transaction per pass (rule 3 allows it) and each response waits for that
  transaction's commit (rule 8's group commit). A write resent after its batch was rolled back had committed
  nothing, so each request still commits exactly one statement.
- Nothing is cached across requests. The HMAC key's padded blocks are computed once at startup, which is a key
  schedule, not a cache of verifications.
- Session settings go in the startup packet, with the same effect as `SET`: `jit=off`,
  `plan_cache_mode=force_generic_plan` and `max_parallel_workers_per_gather=0`.
- The server raises its soft open-file limit to the hard limit at startup.

## Measured

These are not official numbers.

### Latest: 1,000,000 connections with group commit

The server (8 workers, unpinned) ran on a 4-core/8-thread Xeon E3-1230 v3 from 2013, with a 10 GbE port that reaches
the database box at 2.5 Gb/s. Postgres 18.6 ran natively on Linux on an i7-8700K with `box/postgresql.conf`.
`mload -gzip` ran on an i7-2600K and an i7-2630QM laptop. Each step holds 1,000,000 open connections and shortens
think time to raise the rate; "equivalent users" is the official think scale of 2.5 at the same rate.

| Think scale | Req/s at hold | Equivalent users | p95 | p99 | Postgres CPU (of 12 threads) | Server box idle | Result |
|---|---|---|---|---|---|---|---|
| 1.25 | 84,500 | ~2,000,000 | 29.5 ms | 241 ms | ~5.8 | 49% | pass |
| 1.175 | 89,900 | ~2,100,000 | 28.7 ms | 216 ms | ~6.2 | 40% | pass |
| 1.1 | 95,900 | ~2,300,000 | 37.9 ms | 227 ms | ~6.7 | 36% | pass |
| 1.0 | 105,300 | ~2,500,000 | 86.5 ms | 349 ms | ~7.5 | 28% | pass |
| 0.9 | 115,800 | ~2,750,000 | 300 ms | 573 ms | ~8.4 | 15% | pass, marginal |
| 0.8 | 120,400 (~130,000 offered) | ~2,850,000 | 1,094 ms | 1,695 ms | ~8.8 | 0.2% | fail |

The limit here is the server box's CPU: at the failing step the server used about 7.6 of its 8 threads, and softirq
took 33%. Reads and writes slowed down together. The same machines with Postgres under WSL2 failed at about
89,000 req/s, with Postgres as the limit.

### Earlier runs, before group commit

The tables below were measured before writes were group-committed (each write had its own transaction). The server
ran on the same Xeon, pinned to its 4
physical cores with `taskset` (4 workers). Postgres 18.6 ran on an i7-8700K under WSL2 with `box/postgresql.conf`,
over gigabit. `mload -gzip` ran on an i7-2600K (over 10 GbE and gigabit) and, for the two-generator runs, also on an
i7-2630QM laptop (gigabit). None of the machines had `box/setup-app-box.sh` applied.

A real million users needs a million open connections. To offer the same request rate with fewer, these runs shorten
think time: 300,000 users at think scale 0.75, 150,000 at 0.375, 100,000 at 0.25 or 50,000 at 0.125 offer what
1,000,000 offer at 2.5.

| Users, think scale | Equivalent users | Hold | Req/s after ramp | p95 | p99 | Errors | Server CPU | Postgres CPU |
|---|---|---|---|---|---|---|---|---|
| 50,000, 0.125 | 1,000,000 | 1 min | 42,100 | 9–10 ms | 24–33 ms | 0 | 1.31 cores | 3.58 cores |
| 100,000, 0.25 | 1,000,000 | 2 min | 42,200 | 9–10 ms | 35–60 ms | 0 | 1.30 cores | 3.59 cores |
| 150,000, 0.375 (two generators) | 1,000,000 | 5 min | 42,200 | 8–9 ms | 10–38 ms | 0 | 1.31 cores | 3.76 cores |
| 300,000, 0.75 (two generators) | 1,000,000 | 5 min | 42,300 | 8–23 ms | 9–83 ms | 0 | 1.29 cores | 3.78 cores |
| 50,000, 0.0833 | 1,500,000 | 1.5 min | 62,700 | 22–35 ms | 49–66 ms | 0 | 2.09 cores | 6.67 cores |
| 50,000, 0.0625 | 2,000,000 | 1.5 min | 75,000 (84,000 offered) | 312–374 ms | 352–474 ms | 0 | 2.72 cores | 9.30 cores |

p95 and p99 are from the last 5-second samples of each run. Over the whole 300,000-connection run, ramp included,
p95 was 8.9 ms, p99 26.2 ms and the slowest request 288 ms; that run used eight target IPs (four per generator). All
six runs passed `mload`'s verdict. Server RSS was 620 to 650 MB in all (four separate SBCL heaps), and server CPU
did not grow with the number of open connections.

The 2,000,000 row did not reach the offered rate, and the server still had more than a core spare. Postgres was the
limit there: under WSL2 the 8700K tops out at about 9–10 busy threads of 12. My asm entry, run the same day on the same
machines, did the same: 75,000 req/s with 1.79 server cores and 9.96 Postgres cores, and a whole-run p99 of 853 ms
against 413 ms here.

At 1,000,000 equivalent users the asm entry used 0.82 server cores and the same Postgres CPU (3.50 cores), so this
server spends about 1.6× the CPU of the hand-written assembly for the same work.

Killing all 64 Postgres backends at 42,000 req/s failed 2,400 requests (HTTP 500, 0.063% of the run) in one 5-second
window. All 64 connections were back within 10 s and the run passed.

On one 8-core machine over loopback (server, Postgres in Docker and `mload` together), 20,000 users at think scale
0.25 gave 8,450 req/s with p95 1.1 ms and p99 1.8 ms. A profile of one worker there put 38% of samples in the kernel's
`send`, 22% in gzip and CRC-32, and 7% in `recv`.

### Open connections

These runs keep each step at a fixed request rate and raise the number of connections, up to a real 1,000,000 users
at the official think scale of 2.5. Load came from three `mload` processes (two on the i7-2600K, one on the laptop)
over 16 server IPs, 300 s ramp, 3 min hold. Results are for the whole run, ramp included.

| Connections, think scale | Equivalent users | Req/s after ramp | p50 | p95 | p99 | Errors | Server CPU | Server RSS | Postgres CPU | Result |
|---|---|---|---|---|---|---|---|---|---|---|
| 420,000, 1.05 | 1,000,000 | 42,200 | 5.3 ms | 10.1 ms | 98.5 ms | 0 | 1.31 cores | 655 MB | 3.43 cores | pass |
| 500,000, 1.25 | 1,000,000 | 42,400 | 5.4 ms | 11.7 ms | 177 ms | 0 | 1.34 cores | 661 MB | 3.45 cores | pass |
| 600,000, 1.5 | 1,000,000 | 42,300 | 5.5 ms | 14.3 ms | 214 ms | 0 | 1.36 cores | 673 MB | 3.58 cores | pass |
| **1,000,000, 2.5** | **1,000,000** | **42,200** | **5.4 ms** | **13.2 ms** | **208 ms** | **0** | **1.39 cores** | **706 MB** | **3.60 cores** | **pass** |
| 1,000,000, 1.667 | 1,500,000 | 63,500 | 6.4 ms | 50.6 ms | 259 ms | 0 | 2.24 cores | 715 MB | 6.74 cores | pass |
| 1,000,000, 1.25 | 2,000,000 | ~78,000 (84,000 offered) | 234 ms | 2,331 ms | 3,269 ms | 2 timeouts | 3.11 cores | 981 MB | 8.86 cores | fail |

At 1,000,000 connections the machine held 1,000,085 TCP sockets with 5.7 GB of memory in use in all. Server CPU
barely changes with the number of open connections; it follows the request rate.

The p99 near 210 ms in the passing runs comes from the gigabit LAN paths, and it matches the kernel's minimum
retransmit timeout. The `mload` process that reached the server only over 10 GbE kept p99 near 21 ms through the
1,000,000 hold, while the processes over gigabit sat near 214 ms.

The failing run is the rate limit, not the connection count. Postgres was at 8.9 of the 9 to 10 threads the 8700K
gives under WSL2, and the server at 3.1 of its 4 cores, so both were close to their limits, Postgres first.

Rig limits found on the way, all on the load generators: Linux's `connect()` uses only every other port of
`ip_local_port_range` until that half runs out, about 27,800 per server IP and port here. Past that, connecting
slows to a crawl, which stalled one 420,000 run at 223,000 connections; more server IPs fixed it. A single `mload`
process also stops at its open-file limit of 524,288.

Not yet tested: more than 1,000,000 open connections, or hardware like the official boxes.

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
