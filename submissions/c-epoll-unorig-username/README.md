# c-epoll: C, one epoll loop per core, its own Postgres wire client

- Language: C (gcc from Ubuntu 24.04's build-essential, `-O2 -march=x86-64-v2 -flto`)
- Framework / IO: none; raw epoll for readiness, one loop per thread, hand-written HTTP/1.1; the reads and sends of
  a pass are submitted together through io_uring (liburing 2.12, source tarball SHA-256 pinned, linked statically)
- Driver: none; the Postgres v3 wire protocol is spoken directly from `server.c` (about 300 lines: SCRAM-SHA-256,
  Parse once, Bind/Execute per request, DataRow parsing)
- Compression: ISA-L 2.31.1 igzip (level 0), source tarball SHA-256 pinned, linked statically
- Front: direct (no Nginx), ports `PORT` and `EXTRA_PORT`
- Postgres connections: 64 in all (`max(2, 64 / threads)` per thread)

## Design

- **Threads**: one per CPU in the affinity mask (`sched_getaffinity`), so `CPUAffinity`/`taskset` sets it.
  Nothing else is configurable.
- **Listeners**: every thread opens its own `SO_REUSEPORT` listener on **both** ports and accepts into its own
  epoll set; the kernel spreads new connections over the threads. A connection lives on its thread for its
  whole life, so threads share nothing and take no locks.
- **Postgres**: each thread owns its connections, non-blocking, driven by the same epoll loop, and talks the
  wire protocol itself. At connect: StartupMessage, authentication (trust, password or SCRAM-SHA-256, which
  the box uses), then one Parse per statement (exactly the SQL in `SPEC.md`, parameter types given) under one
  Sync. Per request: one Bind (unnamed portal, binary parameters, results binary except a post's `id` and
  `like_count`, which come back as text because "523117" and "0" are shorter than 8 and 4 bytes) and one Execute appended
  to the connection's output buffer; no Describe, so a reply is BindComplete, DataRows, CommandComplete and
  nothing else (libpq's `PQsendQueryPrepared` always adds a Describe, i.e. a RowDescription of ~130 bytes on
  every reply). The buffer goes out in one `write` at the end of the loop iteration after one Sync. Replies
  come back in order and are matched to their requests from a ring; the response is built straight from the
  DataRow bytes in the read buffer, no result object in between. NoticeResponse and ParameterStatus are
  skipped wherever they appear; a connection whose stream stops making sense is dropped like a lost one.
  - Reads: a loop iteration's reads fill one connection's batch (up to 64) and share one Sync, so the batch is
    one write, one round trip and one implicit transaction. The next batch goes to the least-loaded read
    connection. (Spreading each iteration's reads over every connection, as before, cost a round trip per
    connection: 33% more server CPU and 75% more Postgres CPU per request.)
  - Writes go to separate connections (a quarter of each thread's, at least one), so a slow commit never
    holds up reads queued behind it. A loop iteration's writes form one group (up to 128) on one write
    connection under one Sync: one implicit transaction, one commit (group commit). Every write's response
    is held until that Sync's result arrives, which Postgres sends after the commit (rule 8). If a write in
    the group fails (a like on a missing post, `23503`), Postgres rolls the whole group back and skips the
    rest: the failing write gets its own error (404), and the others are sent again, each alone between two
    Syncs, so each commits or fails on its own. A connection lost mid-group answers 500 to all of its writes.
  - **Lost connections.** A connection is lost on a socket error, EOF, a reply that makes no sense, or the
    watchdog: a connection that owes replies and has received no bytes for 10 s (checked about every 500 ms
    from the loop, no extra thread) is closed. Its outstanding reads are sent again on another read connection
    of the thread, at most 3 times per request; its outstanding writes get 500, since their commit may have
    run (a write is never sent twice). It is reconnected through the usual path (one try per second per
    thread, 3 s connect and handshake timeout; the handshake itself is blocking, once per connection). This is
    the pattern of fsharp-kestrel-uring-delneg (#105, merged) and csharp-epoll-jh-d (#59, merged); #78 re-sends
    reads after a Postgres error the same way.
  - **Loss hedging for reads (compiled out by default).** The code is behind `NO_HEDGE`, which `build.sh` defines:
    it is off until the maintainer rules on it (#100). On, it re-sends a stalled connection's reads after 20 ms of
    silence while other connections of the thread are answering; see "Rules notes".
- **HTTP**: requests are parsed in place from a per-thread 64 KB read buffer. While a request waits for
  Postgres its connection is "busy"; pipelined requests behind it wait in a per-connection buffer, so
  responses stay in order. Responses are built in a per-thread buffer and sent in one piece.
  Keep-alive connections are never timed out.
- **Syscall batching**: epoll still reports which sockets are ready, but a pass no longer calls `read` and
  `writev` once per request. Each ready client's read and each ready Postgres connection's read is queued as an
  io_uring RECV and all are submitted with one `io_uring_enter`; their handlers build the responses and fill the
  Postgres batches, which are queued as SEND operations and go out with a second `io_uring_enter`. A pass is
  `epoll_wait`, two submits and the sleep, however many requests it serves. Every operation carries
  `MSG_DONTWAIT`, so it completes inside the submit and its result (bytes, `EAGAIN`, a short send, an error) takes
  the same path the result of `read`/`write` took: the unsent rest of a response waits in the connection's buffer
  for `EPOLLOUT` as before. What keeps it correct:
  - one queued operation per descriptor at a time, so a connection's bytes cannot change order; a pipelined
    request behind a queued response is served when that send's result is in;
  - a descriptor with a queued operation is never closed before its result is handled (a `Connection: close`
    response closes in the send's handler), so a queued send can never reach a descriptor number that `accept4`
    gave to another client in between;
  - a write is still answered only after its group's ReadyForQuery was read and parsed; queueing changes how the
    bytes are handed to the kernel, not when a response is produced.
  If the ring cannot be created (old kernel, a seccomp profile without io_uring) the loop logs it once and uses
  plain `read`/`write`; `-DNO_URING` builds that path alone. No registered files or buffers, no SQPOLL, no
  multishot: epoll stays the event source, the ring only carries the system calls.
- **Batching window**: a loop iteration lasts at least 2 ms, measured from the moment `epoll_wait` returned;
  whatever is left of the 2 ms after serving its events is slept before the next `epoll_wait`. At this load a
  thread otherwise wakes for nearly every request and every Postgres result; with the fixed cadence one wake-up
  serves everything that arrived meanwhile, the reads among it share one Sync and the writes one commit. An
  iteration that filled the 512-event array goes straight on, so the cap on throughput stays where it was. The
  cost is latency: a few ms more per request (p95 0.6 ms -> 5.3 ms locally, against the 500 ms limit).
  `-DBATCH2_NS=4000000` (not set by `build.sh`) makes the window 4 ms when there are one or two loops; measured
  below, it saved a little more CPU but did not raise the load the server could carry, so it stays off.
- **Scheduling**: the loop threads put themselves in `SCHED_IDLE`, the lowest scheduling class, so on the same
  core anything else runs first, above all `ksoftirqd`, which does the receive work once the kernel defers it
  under load. The box's NIC has one queue; receive work that waits behind a busy loop is how its ring
  overflows and packets are lost. It is these threads' own policy: no affinity, no priority raised, nothing
  box-wide.
- **Delayed ACKs**: accepted sockets get `TCP_QUICKACK` 0, and again after any reply later than 30 ms. After a
  user's think time (longer than the RTO) the kernel would otherwise ACK each request at once, a packet the
  response carries anyway.
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
| `TCP_THIN_LINEAR_TIMEOUTS` on client and Postgres sockets | with few packets in flight (always, here) a lost packet is retried every RTO instead of with doubling backoff: a response lost twice waits 400 ms, not 600 |
| `TCP_DEFER_ACCEPT` on the listeners, read right after `accept` | a new connection arrives with its first request already queued, so it is served in the same pass: one wake-up per new connection instead of two |
| Own wire client: Parse once, Bind + Execute per request, no Describe | many requests per round trip; no SQL parsing or planning per request; integers and timestamps without text parsing; no RowDescription in any reply (about 130 of the ~1,500 bytes Postgres sent per request with libpq), and Postgres does not build one |
| Responses built from the DataRow bytes in place | no PGresult: no per-row allocation or copy between the socket and the JSON |
| Separate write connections | a commit (fsync on the database box) only delays other writes, never reads |
| A loop iteration's reads in one batch on one connection, one Sync | one write, one round trip and one ReadyForQuery per batch instead of one per connection: -25% server and -45% Postgres CPU per request |
| 2 ms pause after a light iteration | fewer wake-ups, `epoll_wait` calls and Postgres round trips per request: 44% less server CPU per request at a per-thread rate like 1M users on 16 cores |
| 40-byte connection slot, no per-connection buffers | 1M idle keep-alive connections cost ~40 MB of user memory |
| `created_at` formatted from the binary timestamp | truncates to milliseconds exactly, no time library |
| JSON strings scanned for escapes eight bytes at a time; a timestamp built in a local buffer and copied once | a feed is 20 bodies of ~130 bytes that almost never need an escape: 73 -> 27 ns per post (arm64, measured alone), about 6% of the server's user-mode CPU per request |
| Responses built in place, `Content-Length` back-filled | no allocation per response |
| A pass's reads submitted as one batch of io_uring RECVs, its responses and Postgres batches as one batch of SENDs | two kernel entries per pass for all socket I/O instead of two per request plus one or two per Postgres connection |
| 10 s silence watchdog on Postgres connections, reads re-sent elsewhere | a connection that stops answering (TCP would take minutes to give up) no longer holds its batch of requests to the 60 s client timeout; its reads complete on another connection, its writes answer 500 |
| `SCHED_IDLE` loop threads | the kernel's receive work on a core never waits behind the server |
| `id` and `like_count` as text | ~100 bytes fewer per feed from Postgres, the largest share of the bytes coming into the box |
| `TCP_QUICKACK` 0 | the request's ACK rides on the response |
| ISA-L igzip level 0, only above 1 KB | cuts the feed's bytes ~2.4x; 40% faster than libdeflate level 1, same packet count; small bodies would not shrink |

## Rules notes

- Queries: the five statements are exactly the ones in `SPEC.md` (plus `SELECT 1` for `/health`), one per
  request, prepared, pipelined. Nothing is cached across requests. Writing the client by hand changes what is
  on the wire, not what Postgres runs: the same prepared statements, the same parameters, one Bind/Execute per
  request.
- Re-sent reads: a read whose connection was lost (error, EOF, or 10 s of silence) runs its SPEC statement again
  on another connection, at most 3 times, and only one reply reaches the client; writes on a lost connection
  are never re-sent, they answer 500. Precedent: fsharp-kestrel-uring-delneg's read re-send on a lost connection,
  named in its scoring summary (https://github.com/arjaythedev/million-users-challenge/pull/105#issuecomment-6727280534)
  and merged (https://github.com/arjaythedev/million-users-challenge/pull/105#issuecomment-6739834916);
  csharp-epoll-jh-d (#59, merged) does the same; #78 re-sends reads after a Postgres error
  (https://github.com/arjaythedev/million-users-challenge/pull/78#issuecomment-6621657612). Nothing is reused
  between requests.
- Hedged reads (off): the same re-send after 20 ms of silence instead of 10 s is in `server.c` behind `NO_HEDGE`,
  which `build.sh` defines, so the default build never runs a statement twice unless its connection is lost.
- `SCHED_IDLE` and `TCP_QUICKACK` are settings of this process's own threads and sockets.
- Writes: group commit; every write's response is sent only after the Sync that commits its group (or, after
  a failed group, its own retry) has returned.
- Connections: 64 to Postgres at most (2 per thread if there were more than 32 threads), well under 512.
- Dependencies (rule 12): ISA-L and liburing, each fetched by `build.sh` from a SHA-256 pinned source tarball and
  linked statically. No libpq.
- io_uring is used for this process's own sockets only: no affinity, no socket filter, nothing box-wide.

## Measured locally

Docker on an Apple-silicon Mac (a VM shared with other benchmarks), Postgres 18 in a container with
`box/postgresql.conf` (shared_buffers 2 GB). Treat the numbers as rough.

- Syscall batching A/B (arm64 VM, server on 2 CPUs with 2 loops, mload on 3 others over loopback, Postgres on 7,
  30,000 users, `-gzip -think-scale 0.1`, 40 s hold, about 25,700 req/s, three rounds in rotated order, 0 errors
  in every run). Server CPU per request (total / user / system, us) and latency:

  | Build | CPU per request | p50 / p95 ms | System calls per request |
  |---|---|---|---|
  | before (plain `read`/`writev`) | 9.58 / 2.99 / 6.58 | 3.5 / 5.6 | 2.16 |
  | this build with `-DNO_URING` | 9.45 / 3.01 / 6.43 | 3.5 / 6.0 | |
  | batched (the default) | 8.66 / 2.43 / 6.24 (-9.5%) | 3.8 / 6.2 | 0.11 |
  | batched, 4 ms window (`-DBATCH2_NS`) | 8.37 / 2.32 / 6.05 (-12.6%) | 7.0 / 10.4 | 0.06 |
  | 4 ms window without batching | 9.28 / 2.92 / 6.36 | 6.8 / 9.6 | |

  With the server capped at 0.3 CPU (so it, not Postgres, is the limit) and users raised in steps of 2,000, two
  passes: p95 stayed under 50 ms up to 31,000 users before and 33,000 batched; the last passing step was 35,000
  and 33,000 before, 37,000 and 37,000 batched; throughput at the cap about 24,200 -> 25,500 req/s. The 4 ms
  window passed no more than 2 ms did (35,000) and doubles p50, so it is not the default.
  The loopback VM spends about 9 us per request where the benchmark box spends about 47, most of it kernel
  network work this does not touch, so the share saved there is smaller.
- Checks for the batching: the 42 tests pass with the ring and with the fallback; a consistency check (2,000
  keep-alive connections, no think time, 30 s, every response checked against its request) 4.38M requests, 0
  mismatches (4.04M, 0 with the queue shrunk to 80 operations so that queued and plain I/O mix); 2,400 pipelined
  requests written to a socket that is not read for 2 s (short sends, `EAGAIN`) come back whole and in order,
  the same 10,673,579 bytes as before.
- `test/run.sh`: `passed=42 failed=0` in a clean Ubuntu 24.04 container that ran `install.sh`, for both the
  default build and the hedged one.
- Watchdog (amd64 under Rosetta, server on 4 CPUs, 5,000 users at `-think-scale 0.5`, ~1,000 req/s): at t=25 s
  the busiest 8 Postgres connections (the active read connections of the 4 threads) were silently blackholed in
  both directions with iptables DROP. 10 s later every one was declared lost ("no reply for 10 s"), the 9 reads
  queued on them were answered through another connection (max latency 10.4 s, no timeouts), 0 errors in 71,282
  requests, 64 connections again within a second. Same with the 4 active write connections: the 5 likes in flight
  on them answered 500 (0.007%), every read completed.
- Loss A/B (same box, mload over loopback, `-gzip`, 90 s hold) with `tc netem loss 1%` on the Postgres link only
  (both directions), 20,000 users at `-think-scale 0.5` (~3,840 req/s), two rounds in reversed order: the
  previous version built without hedging p99 207.7 / 207.7 ms (the retransmit timer), this one (watchdog, no
  hedging) 205.7 / 207.7 ms; p95 4.6 ms, 0 errors, the same req/s and CPU (server 10.5 to 10.9%, Postgres 34 to
  36% of a core) in all four. The watchdog never fires at 1% loss: TCP retransmits long before 10 s. With hedging
  compiled in, the same runs give p99 24.5 / 24.2 ms with about 1.2% of reads sent again.
- No injected loss, 40,000 users at `-think-scale 0.3` (~12,400 req/s), hedged build: the version before
  `SCHED_IDLE` p95 5.1 ms / p99 7.4 ms, with it 4.8 / 6.6 ms (so it costs nothing when the box is not contended);
  337 reads hedged in ~1.4M requests.
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
sudo ./install.sh   # build-essential, curl, nasm (ISA-L's x86-64 assembly)
./build.sh          # fetches and verifies the ISA-L and liburing tarballs, builds bin/server
DATABASE_URL=postgres://user:pass@host:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 ./start.sh
```
