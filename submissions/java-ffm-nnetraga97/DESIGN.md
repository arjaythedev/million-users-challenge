# Design: a lean Java server for the Million Users Challenge

Status: build steps 2 (Postgres driver) and 3 (server on epoll) done, with gzip (section 9) and the Postgres
pipeline-depth cap. `test/run.sh` passes 42/42 on macOS (java.nio backend) and on Ubuntu 24.04 (epoll). Step 4 was
a smoke test on one shared 8 vCPU VPS (`perf/`): 300,000 users on 4 server cores with 0 errors, ~52 µs of server
CPU per request, 53–68 MB live heap. Steps 5 (tuning) and 6 (io_uring) are left; the defaults below are submitted.
Reviewed once by GPT-6 Astra (high); its fixes are folded in below.

**Changed while building step 3:** a connection has at most **one** request in flight (not 16). Pipelined requests
wait in its input buffer until the one before is answered. Responses stay in order with no reordering logic, and
nothing in the load test pipelines. A request that finds every Postgres connection at the depth cap (`PG_PIPELINE`,
default 64), or older requests already waiting, is parked: it has been parsed, authenticated and validated, and only
its query (kind and parameters) waits in a FIFO until a connection has room. So it is never checked twice, and a
token can't expire while it waits.
The io_uring backend (build step 6) is optional and set aside for now.
Postgres connects and reconnects are non-blocking (2 s timeout); only the DNS lookup can block, and the JVM caches it.

## Goal

The challenge is per language: the aim is the best Java entry on **all three scores**.

| Cores | Searched up to | Target |
|---|---:|---|
| 16 | 1,000,000 (the cap) | **1,000,000 (max)**: the headline number the leaderboard ranks by |
| 8 | 3,000,000 | as high as possible |
| 4 | 3,000,000 | as high as possible |

On 16 cores the CPU is generous at 1M users; what decides it is holding a million open connections without the
memory or GC collapse that stopped the Java example. On 8 and 4 cores CPU per request decides it as well. In every
case the server's CPU and memory should stay low enough that any run past ~1M users is plainly limited by
Postgres, not by us.

The Java example (Spring Boot, Tomcat, Hikari, pgjdbc) held 321k / 312k / 176k. It died from heap: per-connection
Tomcat state plus in-flight request garbage grew the live heap to 22 GB and G1 spiralled. This design removes both.

## The load, in numbers

From `bench/mload/main.go` with `-think-scale 2.5`: a user's cycle averages 51.25 s and makes 2.17 requests
(`/feed`, `/posts/:id`, a like 15% of the time, a post 2%).

| | per user | at 1M | at 3M |
|---|---|---:|---:|
| Requests/s | 0.0423 | 42,300 | 127,000 |
| `GET /feed` (46%) | | 19,500 | 58,500 |
| `GET /posts/:id` (46%) | | 19,500 | 58,500 |
| `POST /posts/:id/like` (6.9%) | | 2,930 | 8,780 |
| `POST /posts` (0.9%) | | 390 | 1,170 |
| Open keep-alive connections | 1 | 1M | 3M |

The ramp adds at most ~3,300 users/s, so a 3M run is ~15 minutes of ramp plus the 5-minute hold.

**CPU budget per request** (all of our code plus the kernel's TCP and RPS work on the same cores):

| Run | Requests/s | Budget at 100% | At 85% |
|---|---:|---:|---:|
| 16 cores, 1M users | 42,300 | 378 µs | 322 µs |
| 8 cores, 3M users | 127,000 | 63 µs | 53.5 µs |
| 4 cores, 3M users | 127,000 | 31.5 µs | 26.8 µs |

Every 1 µs saved per request is 0.127 core at 3M.

**Memory budget**: 32 GB for everything, the same box for every core count. Kernel state per idle established
socket is unmeasured (guess 3-4 KB: ~3.5 GB at 1M, ~10 GB at 3M). **Every 1 KiB of our own per-connection state
costs 0.95 GiB at 1M and 2.86 GiB at 3M.** Target: under 1.5 KB of process RSS per idle connection (the Rust entry
measured ~1.2 KB). At 1M on 16 cores that leaves plenty of room; the Java example failed there at ~10 KB per
connection plus in-flight garbage.

**The Postgres ceiling.** The feed query takes 0.40 ms in Postgres (`box/postgresql.conf`). At 58,500 feeds/s
that is ~23 Postgres backends busy all the time for feeds alone, on a 16-core database box. The repo's latest
scoring change caps 16 cores at 1M because one Postgres box is the limit past that (it also dropped a proxy and
replica setup that had capped pipelined servers at ~45k req/s). Above ~1M users our score depends on how the organizer adds database capacity (rule 3 and the
scoring section: a database-limited run doesn't count and is rerun with more capacity). What we control is
using as little app-box CPU as possible, so the database is visibly the limit.

## Architecture at a glance

```
            port 80                port 81
               │                      │
   ┌───────────┴──────────────────────┴───────────┐   SO_REUSEPORT: every worker has
   │ worker 0 │ worker 1 │ ...        │ worker N-1 │   its own listener on both ports
   └────┬─────┴────┬─────┴────────────┴────┬───────┘
        │ epoll (later io_uring) via FFM   │          one platform thread per core,
        │ HTTP parse → route → JWT         │          shared nothing on the request path
        │ PG wire protocol (pure Java)     │
        │ DataRow → JSON bytes → send      │
        └──── R read + W write PG conns ───┘          pipelined, one Sync per request
```

- **N workers = `Runtime.availableProcessors()`** (honours the systemd CPU affinity, so 16, 8 and 4 cores need no setting). Measure
  N against N-1, since the kernel's network processing needs the same cores.
- Each worker owns its listeners, its event loop, its Postgres connections and its buffers. No locks, atomics or
  cross-thread handoff on the request path.
- **Language features:** JDK 25, the Foreign Function & Memory API (final since 22) for syscalls and libdeflate,
  plain platform threads, no virtual threads, no framework, no JDBC.

## Components

### 1. Event loop: epoll first, io_uring second

Both sit behind one small interface (`net/Io`) so the rest of the server doesn't change. Only epoll is built.

**epoll (build step 3).** Per worker: one epoll instance, level-triggered `EPOLLIN | EPOLLRDHUP` on client sockets.
Level-triggered so one `read` into the worker's 64 KB scratch buffer is enough. If more is pending, epoll reports
the socket again, so there's no extra read just to get `EAGAIN`. Syscalls per request: `epoll_wait` (amortized),
`read`, `write`, plus the Postgres socket's share (amortized over the batch).

- `struct epoll_event` is **packed, 12 bytes, on x86_64** and 16 bytes on arm64. Lay it out per architecture, or
  Docker on an Apple Silicon Mac will hide the bug.
- The `epoll_event` data field carries the fd; the connection's slot and generation live in user space (section 2).

**io_uring (build step 6, not built; kept only if it would win the full workload).** Per worker: a ring set up with
`SINGLE_ISSUER | DEFER_TASKRUN` (no SQPOLL: a polling kernel thread costs a core we don't have). Multishot accept,
multishot `recv` with a provided-buffer ring (an idle connection owns no buffer, even in the kernel), and batched
submits.

- On kernel 6.8, when the buffer ring runs dry, `recv` completes with `-ENOBUFS` and **the multishot request ends**.
  Check `IORING_CQE_F_MORE` on every completion and re-arm. Refilling the ring alone doesn't restart it.
- Buffers the kernel writes into must be stable native memory (`Arena`-allocated `MemorySegment`s), never heap
  arrays.
- Probe at startup (`io_uring_setup` can be blocked by policy) and fall back to epoll.

### 2. Connections: a slot, a generation, no Java object

As built (`http/Connections`): a connection is a **slot** in per-worker parallel arrays (fd, generation, flags,
connection header, pending input and output), not an object. Slots are dense and reused; `net/FdMap` maps a
socket to its slot, since fds are process-wide and indexing by fd would size every worker's arrays for every
worker's connections. The arrays double as slots are needed.

**Why the generation matters:** a client can disconnect while its query is still in Postgres, and the OS can reuse
its fd (and the worker its slot) for a new client before the result comes back. Every request in flight carries a
tag of (slot, generation), bumped on every close; a result whose tag no longer matches is dropped, never
delivered.

### 3. Buffers and backpressure

As built:

| Buffer | Size | Holds | Released when |
|---|---|---|---|
| Read buffer | 64 KB, one per worker | bytes just read | at once: requests are parsed in place |
| Pending input | per connection, sized to what arrived, at most 64 KB | a partial request, or requests pipelined behind one in flight | handled; an idle connection keeps none |
| Request arguments | per slot (`api/RequestArgs`) | post id, user id; a new post's body and author | the query completes or the connection closes |
| Response buffers | 16 KB per Postgres connection, 4 KB per worker for immediate replies; grow if a body needs it | the response being rendered | reused for the next response |
| Pending output | per connection | unsent bytes of a slow reader | fully written |

Rules:
- Everything a query needs (ids, username, body) is copied out of the read buffer before it is reused.
- **Slow readers:** a short write keeps the rest and waits for `EPOLLOUT`, and the connection handles no new
  request until its output has drained, so a client that sends without reading only fills its own 64 KB of input
  (more closes it).
- **Overload:** past the pipeline cap, queries wait in a FIFO per worker and kind, capped at 65,536 entries; past
  that a request gets 503 `{"error":"overloaded"}`. A waiting query whose client left is dropped.
- **A stuck Postgres connection:** one that doesn't finish connecting, authenticating and preparing within 5 s, or
  gets no response to a query for 30 s, is dropped; its queries fail with 500 and it reconnects.

### 4. Postgres: a pure-Java wire protocol driver

No libpq. Talking the protocol directly means result bytes go from the socket straight into the HTTP response.

**Startup:** StartupMessage, then **SCRAM-SHA-256** (required by the database box's `pg_hba.conf`;
PBKDF2/HMAC from `javax.crypto`, startup only, so allocation is fine). The server's final signature must check out
before AuthenticationOk is accepted, as libpq does; MD5 and cleartext are answered only if the server asks for them,
and there is no TLS (the box network is private). `SET` nothing server-wide (rule 2).

**Prepared statements** (named, prepared once per connection, binary result format):

| Name | SQL (verbatim from SPEC.md) | Params |
|---|---|---|
| `feed` | post object `SELECT` + `ORDER BY p.created_at DESC, p.id DESC LIMIT 20` | none |
| `post` | post object `SELECT` + `WHERE p.id = $1` | int8 |
| `ins_post` | `INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at` | int8, text |
| `ins_like` | `INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING` | int8, int8 |
| `ping` | `SELECT 1` (health) | none |

**One `Bind → Execute → Sync` per request.** Each request is its own implicit transaction, so an error in one (a
like on a missing post) can't roll back or skip its neighbours. Many requests' messages are batched into **one
socket write** per loop iteration: pipelining, not shared transactions.

**When to answer:** on `ReadyForQuery` (`Z`) for that request's `Sync`, for reads and writes alike. For writes that
is the commit point (rule 8). `CommandComplete` says `INSERT 0 1` (new like, 201) or `INSERT 0 0` (repeat, 200).
`ErrorResponse` with SQLSTATE `23503` maps to 404 `post not found`; anything else is 500.

**Connections:** default **64 read + 64 write in total**, split over the workers (16 cores: 4 + 4 each; 4 cores: 16 + 16 each; 8 cores:
8 + 8), as the Rust entry does, then tuned by measurement. Writes get their own connections so a feed never waits
behind a WAL flush. Each query goes to the worker's connection with the fewest outstanding requests, with a cap on
pipeline depth (start at 64), past which requests queue in the worker. Env: `DB_READ_CONNS`, `DB_WRITE_CONNS`;
total including reconnects stays ≤ 512 (rule 5).

**A dropped connection:** fail its outstanding reads with 500. Fail outstanding writes with 500 too, and **never
retry them**: the commit outcome is unknown. Reconnect in the background (50 ms, backing off to 1 s), re-prepare,
rejoin.

**Decoding:** a message parser that handles messages split across reads. `DataRow` columns are decoded straight
into JSON output: `int8`, `text` (already UTF-8), `timestamptz` (binary microseconds since 2000-01-01 UTC), `int4`.

### 5. HTTP/1.1

A small, real parser, not one shaped to the load generator (rule 9):
- Request line plus headers; read `Content-Length`, `Authorization`, `Accept-Encoding`, `Connection`, `Expect`.
- Keep-alive by default with no idle timeout (the spec needs ≥ 75 s). `Connection: close` and HTTP/1.0 handled.
- Pipelined requests answered in order: one at a time per connection, the rest wait in its input buffer.
- `Expect: 100-continue` answered. Bodies up to 16 KB. Chunked request bodies → 400 (out of scope; nothing sends
  them).
- Unknown path → 404 `not found`. Wrong methods on known paths are out of scope.
- Responses: one `write` carrying headers and body. `Content-Type: application/json`, `Content-Length`, plus
  `Content-Encoding: gzip` when used. `TCP_NODELAY` on every client socket. Listen backlog 65535.

### 6. Request handling and validation (exact rules from SPEC.md and test.sh)

- **Order on authenticated routes: auth first, then the id.** `POST /posts/abc/like` without a token is 401, with
  a valid token 400.
- **Post id:** a positive decimal integer only; `0`, `-1`, `abc`, `1.5` → 400 `invalid post id`.
- **POST body:** a real JSON parser (strings with `\"`, `\\`, `\uXXXX` and surrogate pairs; nested values skipped
  correctly). Invalid JSON → 400 `malformed JSON body`. `body` missing, not a string, or empty after trimming →
  400 `body is required`. Over 500 characters after trimming → 400 `body must be at most 500 characters`. Count
  **code points**, matching Postgres's `char_length` check.
- **Errors:** `{"error":"<message>"}`, messages exactly as in the SPEC table.

### 7. JWT (HS256)

- `Authorization` missing or not starting with `Bearer ` → 401 `missing bearer token`.
- Split on `.`, base64url-decode the header and payload into scratch bytes, check the header's `alg` is exactly
  `HS256`, verify the HMAC in constant time, check `exp` is a number greater than now (and `nbf` if present). Any
  failure, including malformed input → 401 `invalid or expired token`.
- Then the payload: `sub` must be a string holding a positive integer, `username` must be a string → otherwise 401
  `invalid token payload`.
- **Speed-up:** precompute the HMAC's inner and outer key states once (the secret is fixed). That saves 2 of the 6
  SHA-256 blocks per check (tokens are ~135 bytes). Benchmark a reused `javax.crypto.Mac` first; hand-roll SHA-256
  only if profiling says so. Nothing about a token is remembered between requests (rule 9).

### 8. Rendering JSON

- Hand-written, keys in the SPEC's exact order, compact, no trailing spaces.
- Strings escape `"`, `\` and control characters below 0x20; non-ASCII is copied as UTF-8 bytes.
- `created_at`: microseconds → `floorDiv` to milliseconds (truncate, never round), then days → year/month/day with
  a branch-light civil-from-days routine, formatted as `YYYY-MM-DDTHH:MM:SS.mmmZ`. No `java.time` on the hot path.
- `/health`: `{"status":"ok","db":"ok","uptime_s":N}` after `ping` succeeds; 503
  `{"status":"degraded","db":"unreachable","error":"..."}` if it fails or no read connection is up.

### 9. gzip

- When the request sends `Accept-Encoding: gzip` and the body is ≥ 512 bytes (in practice: `/feed`), compress with
  **libdeflate** via FFM, level 1, one compressor per worker. One-shot: the body is copied into the compressor's
  native buffer and the result into a second response buffer that then swaps with the first. (Passing the Java
  arrays straight through a critical downcall allocated two segment views per call whenever the JIT didn't
  eliminate them, so it wasn't reliably allocation-free.) Sent uncompressed when the result isn't smaller.
- libdeflate from Ubuntu 24.04's `libdeflate0` package via apt in `install.sh` (allowed: rule 12). Fall back to
  `java.util.zip.Deflater` when it isn't found, e.g. on a Mac.
- Tune level and threshold on both feed shapes: seed rows (5.1 KB → ~2 KB) and mload's short generated posts
  (~3.7 KB → ~0.8 KB), since generated posts take over the feed during a run. Env: `GZIP_LEVEL`, `GZIP_MIN`.

### 10. The JVM

- **JDK 25.** `install.sh` installs Ubuntu 24.04's `openjdk-25-jdk-headless` from apt, as the challenge's own Java
  example does for JDK 21 (25.0.4.1 when this was built), and `libdeflate0`.
- `start.sh`: `java --enable-native-access=ALL-UNNAMED -XX:+UseParallelGC -Xms1g -Xmx4g` (`HEAP` overrides the 4g).
  ParallelGC because with almost no allocation on the request path young collections are tiny and rare (two of
  ~20 ms in a 5-minute, 300k-user run). Nearly all memory is kernel socket state.
- No Epsilon GC (a leak means running out of memory, and a crashed server isn't restarted), no GraalVM native image
  (the JIT is warm long before the 15-minute ramp ends).
- Hot path: allocation-free in the steady state. Startup, reconnects and error paths may allocate.
- FFM: `read`, `write` and `epoll_wait` capture `errno` with `Linker.Option.captureCallState`, which can't be combined
  with `Linker.Option.critical`, so socket bytes go through one native buffer per worker (a few hundred bytes per
  request, far cheaper than the syscall).

## Rules checklist

| Rule | How this design meets it |
|---|---|
| 2: Postgres only, no server-wide settings | No caches or other stores; no `ALTER SYSTEM`/`ALTER ROLE ... SET` |
| 3: spec SQL, one query per request | Five prepared statements verbatim from SPEC.md; no JSON built in SQL; pipelining only |
| 4: schema fixed | Nothing created, altered or dropped |
| 5: ≤ 512 Postgres connections | 64 + 64 by default; hard cap including reconnects |
| 6: test suite 100% | Build step 3 gate: `test/run.sh` 42/42 on both ports |
| 7: no caching across requests | Every request runs its query; nothing about results or tokens is kept |
| 8: committed before responding | Answer only after `ReadyForQuery` for that request's `Sync` |
| 9: every token checked | Signature, `alg`, `exp` on every request |
| 11: serve directly on 80 and 81 | Own listeners on both ports, no proxy |
| 12: built from source on the box | `install.sh` (JDK, libdeflate via apt), `build.sh` (Maven wrapper, pinned deps) |
| 13: no box tuning | Only in-process settings: socket options, thread and pool sizes, JVM flags |

## Build order

1. **Environment.** An x86_64 Linux VPS (Ubuntu 24.04) with Postgres 18, `seed/make-seed.sh`, Go 1.27 and `mload`.
   A Mac is fine for unit tests; io_uring and epoll need Linux, and the struct layouts need x86_64.
2. **Postgres driver.** Startup, SCRAM, prepare, pipelined Bind/Execute/Sync, binary decode, error and reconnect
   handling. Tested on its own against the seed database, including split messages and a mid-pipeline 23503.
3. **Server on epoll.** Slots and generations, the HTTP parser, routing, JWT, rendering. Gate: `test/run.sh` 42/42.
4. **Measure.** Process RSS and whole-box memory per idle connection (`ss -s`, `/proc/meminfo`, `slabtop`), CPU
   per request (`pidstat`, `perf`), mload at 50k → 200k users from several source IPs. Then a full 1M-user run
   on 16 cores, the headline score; that needs several load-generator machines and source IPs (bench/README.md).
5. **Tune.** gzip level and threshold, read/write connection counts, pipeline depth, worker count, GC, on 16, 8 and
   4 cores (restrict the server with `taskset` or systemd `AllowedCPUs` the way the scoring does).
6. **io_uring backend.** Same server behind the `Io` interface; keep it only if it wins the full workload on
   all three core counts, or wins on 8 and 4 without costing the 16-core million.
7. **Submit.** README with every version and setting, the three scripts, `.gitignore`, and the folder renamed to
   match the stack (see open questions).

## Risks

| Risk | Mitigation |
|---|---|
| Postgres is the limit above ~1M, not us | Minimize our CPU so it's obvious; the organizer reruns database-limited runs |
| Kernel memory per socket is higher than guessed | Measure in build step 4 before tuning anything else |
| Too few Postgres connections serialize the load | Start at 64 + 64, measure queue delay per connection |
| io_uring brings no gain | It's build step 6 and optional; epoll is the baseline; judge it on all three core counts |
| Custom protocol code has edge-case bugs (split messages, errors mid-pipeline) | Driver tested on its own first; fault-injection tests |
| Scope: no libraries for the driver or the event loop | ~2.5-4k lines; build in the order above so each step is testable |
| Bench README says writes are ~5%; the code gives 7.8% | Size for 7.8% |

## Open questions

- **Answered.** The folder is named for the stack (`java-ffm-…`). Ubuntu 24.04 ships `openjdk-25-jdk-headless` 25.0.4.1 and
  libdeflate 1.19 (`libdeflate0`). On kernel 6.8 with the box's settings, slab grew ~4 KB per open TCP socket and an
  idle socket held almost no buffer memory (`perf/run.sh` at 300k users: +2.4 GiB slab for 600k sockets, +33 MiB of
  TCP buffers).
- Whether io_uring is allowed by the box's kernel policy (the systemd unit has no syscall filter, so probably yes).
