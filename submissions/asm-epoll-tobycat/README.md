# asm-epoll-tobycat: x86-64 assembly, one epoll loop per CPU

- Language: x86-64 assembly (GNU as, Intel syntax), statically linked, no libc and no CRT, raw syscalls only
- Framework / IO: none; epoll, hand-written HTTP/1.1
- Driver: hand-written Postgres wire protocol with SCRAM-SHA-256, prepared statements, pipelining and binary results
- Compression: hand-written gzip (greedy LZ77, one fixed-Huffman block)
- Front: direct, on `PORT` and `EXTRA_PORT`
- Dependencies: GNU binutils only

## Design

- **Threads:** one per CPU in the affinity mask. `THREADS` overrides it.
- **Nothing shared on the request path:** each thread has its own `SO_REUSEPORT` listeners on both ports, its own epoll
  set, its own fd table (`unshare(CLONE_FILES)`) and its own Postgres connections. A connection stays on its thread.
- **Postgres connections:** 64 in all (`max(2, 64 / threads)` per thread), a quarter of them for writes.
- **Reads:** a pass over the epoll events fills one connection's batch, up to 64 requests sharing one Sync, so a batch is
  one write and one round trip. The next batch goes to the read connection with the least outstanding.
- **Writes:** each has its own Sync and so its own transaction. The response is built at `CommandComplete` but sent
  only at that Sync's `ReadyForQuery`, when the transaction has committed; an error in between answers 500. A like on
  a missing post fails alone. Writes are not batched into shared transactions.
- **Batching window:** after a pass of fewer than 32 events, the thread sleeps 2 ms (`BATCH_US`) so the next pass serves
  everything that arrived meanwhile. A busy thread never sleeps.
- **Responses:** built in the connection's scratch buffer with room left for headers, then sent with one `write`. Feed
  bodies of 1 KB or more are gzipped when the client accepts it (`HTTP_GZIP=0` turns this off).
- **Memory:** a 64-byte slot per fd, touched only when used. Buffers exist only while a request or response is split.
- **Validation:** strict JSON, HS256 signature and required `exp` on every authenticated request, and `String.trim`
  whitespace including the Unicode spaces.

## Losing Postgres

A lost connection fails the requests queued on it with 500 and is retried once per second per thread, with a 3 s limit on
connecting and on each login step. While a thread has no usable connection it answers 503, and `/health` returns the
spec's `degraded` body. A thread blocks while it logs back in. If Postgres is unreachable at startup, the server exits.

## Rules notes

- The statements are the ones in `SPEC.md` (plus `SELECT 1` for `/health`), one per request, prepared and pipelined.
  The SQL is verbatim; Postgres infers the parameter types (`bigint`, `text`) from the columns.
- Nothing is cached across requests.
- Sessions set `jit=off`, `plan_cache_mode=force_generic_plan` and `max_parallel_workers_per_gather=0` with `SET`.

## Measured

These are not official numbers. The server ran on a 4-core/8-thread Xeon E3-1230 v3 from 2013, Postgres 18.6 on an
i7-8700K under WSL2 with `box/postgresql.conf`, and `mload` on an i7-2600K, all over gigabit. None of the machines had
`box/setup-app-box.sh` applied.

A real million users needs a million open connections. To offer the same request rate with fewer, these runs shorten
think time: 200,000 users at think scale 0.5, 100,000 at 0.25 or 50,000 at 0.125 offer what 1,000,000 offer at 2.5.

| Server CPUs | Users, think scale | Equivalent users | Hold | Req/s after ramp | p95 | p99 | Errors |
|---|---|---|---|---|---|---|---|
| 4 cores | 50,000, 0.125 | 1,000,000 | 5 min | 42,100 | 7–8 ms | 24–38 ms | 0 |
| 4 cores | 200,000, 0.5 | 1,000,000 | 5 min | 42,200 | 6–35 ms | 7–174 ms | 0 |
| 4 cores | 100,000, 0.25 | 1,000,000 | 2 min | 42,200 | 7 ms | 37–61 ms | 0 |
| 4 cores | 50,000, 0.0833 | 1,500,000 | 2 min | 62,700 | 19–25 ms | 49–61 ms | 0 |
| 2 cores | 50,000, 0.125 | 1,000,000 | 2 min | 42,000 | 9–12 ms | 28–53 ms | 0 |

p95 and p99 are from the last three 5-second samples of each run.

At 42,000 req/s with 200,000 connections, the server used 0.72 cores and the whole machine 1.01, kernel network work
included. `taskset` confined only the server's threads, so kernel work could run on the other cores. RSS was about 139 MB.
Over the whole 200,000-connection run, ramp included, p95 was 7.6 ms and p99 119 ms.

During that hold, p95 moved between 6 ms and about 35 ms in spells lasting tens of seconds. The cause is not established.

With all connections on one target IP, a 100,000-connection run stalled for about 25 s during the ramp and failed (p99
3,019 ms). Spreading the same load over four target IPs removed the stall, so this was the test setup rather than the server.

Killing all 64 Postgres connections under full load caused one 5-second window of errors (500s and 503s, 0.59% of
the run) and then normal service. All connections returned.

Limits found on this rig, each with `BATCH_US` at its default and `mload` over four target IPs or 50,000 users:

| Server CPUs | Equivalent users | Req/s after ramp | p95 | p99 | Whole-run result |
|---|---|---|---|---|---|
| 1 core, 1 thread | 1,250,000 | 52,400 | 13–19 ms | 35–53 ms | pass |
| 1 core, 1 thread | 1,500,000 | 62,500 | 27–29 ms | 48–54 ms | pass |
| 1 core, 1 thread | 1,750,000 | 71,000 | 61–63 ms | 246–251 ms | pass |
| 1 core, 1 thread | 2,000,000 | 71,500 (84,000 offered) | 316–325 ms | 556–591 ms | pass, near the limit |
| 4 cores | 2,000,000 | 71,400 (84,000 offered) | 409–456 ms | 567–639 ms | pass, near the limit |

These runs are 1 to 1.5 minutes, not 5-minute holds. The last two rows did not reach the offered rate: the gigabit link to
Postgres ran at up to 119 MB/s, so they do not measure the server's own limit. The 1-core rows also let kernel network work run on
the other cores (the whole machine used 1.82 cores at 71,500 req/s), so they are not what a 1-core or 4-core box would do.

The largest connection count tried was 420,000 at 42,000 req/s. The server held it for the whole hold with no failed requests
in steady state and the machine's used memory up by about 1 GB, but the run failed `mload`'s verdict (whole-run p99 21,870 ms,
3 timeouts). During the ramp, the load generator's kernel load rose and connections stalled, while the server box stayed under
one core of load. During the hold, p95 was mostly 8 ms with spells near 230 ms and p99 218 to 421 ms.

With two load generators (200,000 + 100,000 connections, think scale 0.75, 100 s ramp) a 5-minute hold of 300,000 connections
passed on 4 cores at 42,300 req/s, with no failed requests. Server RSS was 214 MB, and the whole machine used 1.03 cores.

Repeating 420,000 as 320,000 + 100,000 failed again on the larger generator, which stalled at about 240,000 connections. The
smaller generator, measuring the same server during that stall, kept p95 at 8.4 ms and passed on its own (p99 208 ms). So the
ramp stall is in the load generator, not the server. At 420,000 connections, p99 near 210 ms lines up with more TCP
retransmits than at 300,000, which is about the size of the kernel's minimum retransmit timeout. Where the packets are lost
is not established.

Not yet tested: a clean run above 300,000 connections, a real 1,000,000, and any run on hardware like the official boxes.

## Running

```bash
sudo ./install.sh   # binutils
./build.sh          # assembles and links ./server
DATABASE_URL=postgres://user:pass@ipv4:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 ./start.sh
```

`DATABASE_URL` may name the host by IPv4 address or name: `/etc/hosts` first, then an A query to the nameservers in
`/etc/resolv.conf`, once at startup. IPv6 and resolv.conf's search domains are not supported. User, password and database
are percent-decoded. Authentication must be SCRAM-SHA-256 or trust.

## Credits

The design follows `c-epoll-unorig-username` in this repo: per-thread loops, one Sync per read batch, separate write
connections and the batching window. The code is written separately and grew out of my entry in the multicore challenge.
