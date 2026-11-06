# c-epoll: C, one epoll loop per core, libpq in pipeline mode

- Language: C (gcc from Ubuntu 24.04's build-essential, `-O2 -march=x86-64-v2 -flto`)
- Framework / IO: none; raw epoll, one loop per thread, hand-written HTTP/1.1
- Driver: libpq 18.6, built from the PostgreSQL 18.6 source tarball (SHA-256 pinned) and linked statically
- Compression: ISA-L 2.31.1 igzip (level 0), source tarball SHA-256 pinned, linked statically
- Front: direct (no Nginx), ports `PORT` and `EXTRA_PORT`
- Postgres connections: 64 in all (`max(2, 64 / threads)` per thread)

## Design

- **Threads**: one per CPU in the affinity mask (`sched_getaffinity`), so `CPUAffinity`/`taskset` sets it.
  Nothing else is configurable.
- **Listeners**: every thread opens its own `SO_REUSEPORT` listener on **both** ports and accepts into its own
  epoll set; the kernel spreads new connections over the threads. A connection lives on its thread for its
  whole life, so threads share nothing and take no locks.
- **Postgres**: each thread owns its connections, all in libpq pipeline mode and non-blocking, driven by the
  same epoll loop. A request is one prepared statement (exactly the SQL in `SPEC.md`, binary results);
  results come back in order and are matched to their requests from a ring.
  - Reads: a loop iteration's reads fill one connection's batch (up to 64) and share one Sync, so the batch is
    one write, one round trip and one implicit transaction. The next batch goes to the least-loaded read
    connection. (Spreading each iteration's reads over every connection, as before, cost a round trip per
    connection: 33% more server CPU and 75% more Postgres CPU per request.)
  - Writes go to separate connections (a quarter of each thread's, at least one) and each gets its own Sync,
    so it is its own transaction: a like on a missing post fails alone (`23503` -> 404), and a slow commit
    never holds up reads queued behind it. The response is held until that Sync's result arrives, which
    Postgres sends after the commit (rule 8). No write batching into shared transactions.
  - A lost connection fails its queued requests with 500 and is reconnected (one try per second per thread,
    3 s connect timeout).
- **HTTP**: requests are parsed in place from a per-thread 64 KB read buffer. While a request waits for
  Postgres its connection is "busy"; pipelined requests behind it wait in a per-connection buffer, so
  responses stay in order. Responses are built in a per-thread buffer and written with one `writev`.
  Keep-alive connections are never timed out.
- **Batching window**: after a loop iteration that handled fewer than 32 events, the thread sleeps 2 ms before
  the next `epoll_wait`. At this load a thread otherwise wakes for nearly every request and every Postgres
  result; after the pause one wake-up serves everything that arrived meanwhile, and the reads among it share one
  Sync. A busy thread (32 events or more) never sleeps, so the cap on throughput stays where it was. The cost is
  latency: a few ms more per request (p95 0.6 ms -> 5.3 ms locally, against the 500 ms limit).
- **gzip**: when the request accepts gzip, bodies of 1 KB or more (in practice only `/feed`, 5.3 KB -> 2.2 KB)
  are compressed with ISA-L igzip level 0 (6.4 us per feed here; libdeflate level 1 took 10.3 us for 1.8 KB, and
  either way the response is two TCP segments). Smaller ones are sent plain.
- **Validation** (strict JSON, HS256 JWT with signature and a required `exp` checked on every request, body trimming as
  JavaScript's `String.trim`, ids) is the same code as this author's C entry in the $12 challenge.

## Memory per connection

- User space: one 40-byte slot per descriptor in a table sized by `RLIMIT_NOFILE` (up to 16M; the box's
  `LimitNOFILE=8388608` allows 8.4M connections), allocated with `calloc` (pages are touched only when used, so
  1M connections is 40 MB). Buffers exist only while a request or
  response is split across reads or writes.
- Measured: RSS 13 MB with 100,000 open keep-alive connections, 26 MB with 300,000.
- The rest is the kernel's socket memory (a few KB per idle socket), which no user-space choice changes.

## Optimizations, and why

| What | Why |
|---|---|
| One epoll loop per core, SO_REUSEPORT per thread | no locks or cross-thread hand-off on the request path |
| libpq pipeline mode, prepared statements, binary results | many requests per round trip; no SQL parsing or planning per request; integers and timestamps without text parsing |
| Separate write connections | a commit (fsync on the database box) only delays other writes, never reads |
| A loop iteration's reads in one batch on one connection, one Sync | one write, one round trip and one ReadyForQuery per batch instead of one per connection: -25% server and -45% Postgres CPU per request |
| 2 ms pause after a light iteration | fewer wake-ups, `epoll_wait` calls and Postgres round trips per request: 44% less server CPU per request at a per-thread rate like 1M users on 16 cores |
| 40-byte connection slot, no per-connection buffers | 1M idle keep-alive connections cost ~40 MB of user memory |
| `created_at` formatted from the binary timestamp | truncates to milliseconds exactly, no time library |
| Responses built in place, `Content-Length` back-filled | no allocation or copy per response |
| ISA-L igzip level 0, only above 1 KB | cuts the feed's bytes ~2.4x; 40% faster than libdeflate level 1, same packet count; small bodies would not shrink |
| libpq built without TLS, GSSAPI, zlib, ICU, readline | the database link is plain TCP (`ssl = off` on the box) |

## Rules notes

- Queries: the five statements are exactly the ones in `SPEC.md` (plus `SELECT 1` for `/health`), one per
  request, prepared, pipelined. Nothing is cached across requests.
- Writes: each write commits in its own transaction before its response is sent.
- Connections: 64 to Postgres at most (2 per thread if there were more than 32 threads), well under 512.

## Measured locally

Docker on an Apple-silicon Mac (a VM shared with other benchmarks), Postgres 18 in a container with
`box/postgresql.conf` (shared_buffers 2 GB). Treat the numbers as rough.

- `test/run.sh`: `passed=42 failed=0` in a clean Ubuntu 24.04 container that ran `install.sh`.
- Batching reads on one connection and igzip, native arm64, mload over loopback (server on 4 CPUs, load
  generator on 8 others), 60,000 users, `-gzip`, 90 s hold, server CPU per request in the hold (user + sys):
  - `-think-scale 0.18`, ~35,200 req/s (1M users on 4 cores): 21.7 -> 14.2 to 14.6 us per request; Postgres
    CPU 65-68 -> 37-39 us per request; p95 6-22 -> 8 ms.
  - `-think-scale 0.1`, ~62,500 req/s: 17.9 -> 13.2 us per request, p95 519 -> 18 ms.
  - A consistency check (2,000 keep-alive connections, no think time, every response checked against its
    request): 3.2M requests, 0 mismatches.

  The runs below are from before those two changes (amd64 under Rosetta).
- Batching window A/B, mload over loopback (server on 4 CPUs, load generator on 8 others), 60,000 users,
  `-gzip`, 90 s hold, server CPU per request in the hold (user + sys):
  - `-think-scale 0.5`, ~12,700 req/s (about the per-thread rate of 1M users on 16 cores): 57 -> 32 us per
    request (three runs each), p95 0.6 -> 5.3 ms, p99 0.9 -> 6.9 ms.
  - `-think-scale 0.18`, ~35,200 req/s (1M users on 4 to 5 cores): 1.90 -> 0.79 cores, 54 -> 23 us per request,
    p95 0.8 -> 5.8 ms, p99 14 to 16 -> 8 ms (two runs each).
  - `-think-scale 0.1`, ~62,500 req/s (near the VM's limit): 2.15 -> 1.23 cores, the same throughput, p95
    52 -> 28 ms: a busy thread does not pause, so the window does not cap throughput.

  The runs below are from before the batching window.
- mload, 100,000 users over both ports and 4 target IPs, `-gzip -think-scale 2.5`, 2 min hold, server on 4
  CPUs: 972 req/s per load generator x 4, 0 errors, reads p95 3.4 ms / p99 12.7 ms; writes are slow here
  (the VM's disk fsync, p95 ~2 s). Server RSS 13 MB, 0.32 cores.
- mload, 300,000 users: all 300,000 connections held with 0 errors, RSS 26 MB, 0.55 cores at ~9,100 req/s
  (latency in that run was dominated by the shared VM stalling the load generators).
- wrk, 1 CPU: `GET /posts/:id` 91,000 req/s at 0.95 cores (about 0.010 cores per 1,000 req/s); the
  100 feed : 100 post : 15 like : 2 create mix with gzip, 25,000 to 35,000 req/s at 0.83 to 0.87 cores
  (about 0.025 to 0.033 cores per 1,000 req/s). Postgres, not the server, was the limit in every run.

## Running

```bash
sudo ./install.sh   # build-essential, curl, bison, flex (libpq's configure), nasm (ISA-L's x86-64 assembly)
./build.sh          # fetches and verifies the two tarballs, builds bin/server
DATABASE_URL=postgres://user:pass@host:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 ./start.sh
```
