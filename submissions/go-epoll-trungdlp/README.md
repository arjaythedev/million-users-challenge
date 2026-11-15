# Go 1.27: own epoll loops, pipelined Postgres wire protocol

Feed API submission for `trungdlp`.

| Component | Version |
| --- | --- |
| Go | 1.27.3, CGO disabled, Linux only |
| I/O | own epoll loop per core (`golang.org/x/sys/unix` v0.49.0), hand-written HTTP/1.1 |
| PostgreSQL | `pgconn` (pgx v5.11.0) connects, authenticates and prepares; the loop then speaks the extended protocol on the socket itself |
| JSON | responses written by hand from the result rows; request bodies and JWT claims decoded by github.com/bytedance/sonic v1.15.5 |
| Compression | own gzip encoder (`deflate.go`): greedy LZ77, Huffman tables rebuilt from a live body every 256 bodies, bodies of 1 KB or more |

## Design

- **Loops**: one per `GOMAXPROCS` (Go reads the CPU affinity mask and cgroup limit), each
  on its own locked OS thread with its own epoll set and its own `SO_REUSEPORT` listener
  on both ports. A connection stays on the loop that accepted it; loops share nothing on
  the request path and take no locks. No goroutine per connection or per request.
- **Connection table**: one 8-byte slot per descriptor (generation, loop, kind, flags) in a
  table sized by `RLIMIT_NOFILE` (raised to its hard limit, capped at 16M). It holds no
  pointers, so the garbage collector never scans it; pages are touched only where
  descriptors exist. Buffers exist only while a request or response is split across
  reads or writes. The generation is bumped on close, so a result that arrives after its
  client left never reaches a new connection on the same descriptor.
- **HTTP**: requests are parsed in place from the loop's 64 KB read buffer. While a request
  waits for Postgres its connection is busy and later pipelined requests wait in order.
  A response is built in one buffer and sent with one `write`. Keep-alive connections are
  never timed out. Reads and writes on the non-blocking sockets are raw system calls,
  skipping the scheduler's syscall bookkeeping; only `epoll_wait` blocks.
- **ACKs**: `TCP_QUICKACK` is cleared on the listeners after `listen()`, so even a
  connection's first request has its ACK carried by the response (tcpdump: one packet
  fewer per new connection; later requests already did this). A response that took 25 ms
  or more clears it again on that socket, since a fired delayed-ACK timer leaves the mode.
- **Accept**: the listeners set `TCP_DEFER_ACCEPT`, so a connection is handed over once its
  first request has arrived, and the loop reads it right after `accept`: one wake-up per
  new connection instead of two (as c-epoll-unorig-username and java-epoll-stohirov do).
- **Retransmissions**: client and Postgres sockets set `TCP_THIN_LINEAR_TIMEOUTS`, so while
  fewer than 4 packets are in flight a packet lost again waits one RTO (200 ms at least)
  each time instead of 200, 400, 800 ms. With 10% loss on the server's egress (netem),
  20,000 users: p95 286 to 405 ms down to 244 to 246, p99 633 to 897 ms down to 438 (two
  runs each). Same CPU; no difference at 5% loss, where a packet is rarely lost twice.
  Locally, at about 8k req/s per loop, a request costs 1.3 segments in and 1.1 out on the
  client side; Postgres sends about 1.5 KB per request (the SPEC's rows), in 0.3 packets.
- **Postgres**: 496 connections whatever the core count (the limit is 512), 448 for reads
  and 48 for writes, split over the loops (28+3 each on 16 cores, 56+6 on 8, 112+12 on 4).
  Most are idle: an idle read connection is taken first-fit, so the same few backends
  carry the load. The rest are there for packet loss (see "Packet loss" below).
  `pgconn` connects, authenticates and prepares the SPEC's statements; the socket is then
  hijacked into the loop's epoll set. A request is one Bind+Execute of its prepared
  statement with binary parameters. Results come in whichever format is smaller per
  column: `created_at` binary (8 bytes, not 29), ids and like counts as digits (shorter
  than 8- and 4-byte integers, and copied into the JSON as they are). That is 3.6% fewer
  bytes from Postgres per request (1,499 to 1,445 locally).
  - The feed's `LIMIT 20` is the SPEC's literal. An earlier version bound it as `$1`, and
    Postgres then planned the feed afresh on every execution (only custom plans, never
    the generic one): 0.096 ms per feed against 0.047 ms (pgbench, one client), and 35 to
    38% more Postgres CPU under load.
  - A loop iteration's reads go out in small batches, each on an idle read connection of
    its own under one Sync: at most 2 feeds (about two packets each), or 8 of the small
    reads (a post is about 200 bytes, so 8 share one packet).
  - Writes are group-committed (rule 8 allows it): the writes of a loop iteration go to
    the less-loaded write connection and share one Sync, so one implicit transaction and
    one commit. Each response is held from its CommandComplete until the ReadyForQuery
    after that Sync, where the transaction commits (rule 8). A commit error, or a
    connection lost in between, answers 500. A commit never delays a read.
  - A failed write (a like on a missing post, `23503` -> 404) aborts its batch: Postgres
    rolls back the writes before it and skips those after it. Those run again, each in a
    transaction of its own, so one failure costs every write of its batch at most one
    retry. With 1,800 concurrent likes, a third on missing posts, every status was right
    and the new rows matched the 201s exactly. Under `mload` (Postgres on 2 CPUs, server
    on 4, 50,000 users) group commit took Postgres CPU from 115 to 102 s and write p99
    from 33 to 17 ms.
  - With one write connection, a lost packet held every write of the loop for a
    retransmission; with two, the writes behind it go to the other. At 10% egress loss
    (netem, 20,000 users, two runs each) like p95 went from 359 to 366 ms down to 225 to
    246, create p95 from 342 to 348 down to 218 to 227, reads unchanged.
  - A lost connection fails its queued requests with 500 (`/health`: 503) and is
    reconnected in the background.
- **Batching window**: a loop iteration lasts at least 2 ms from the moment `epoll_wait`
  returned; what is left of it after serving the events is slept (`nanosleep` on the
  loop's own thread; `time.Sleep` needed another thread to wake it). The next iteration
  serves everything that arrived meanwhile. An iteration that filled the event array goes
  straight on, so the window costs latency, not throughput.
- **Scheduling**: the loop threads put themselves in `SCHED_IDLE`, so the kernel's receive
  work on a core (`ksoftirqd`) never waits behind a loop. Their own policy only: no
  affinity, nothing box-wide (as c-epoll-unorig-username does).
- **Packet loss**: the scored holds sit at p95 216 to 225 ms, the retransmission timer, and
  the runs above them fail with reads taking seconds while writes stay near 220 ms. Random
  loss on everything the server receives (netem on the host side of its veth, offloads
  off, 4 CPUs, 30,000 users at `-think-scale 0.1`, about 27k req/s) reproduces both: 5%
  gives p95 216 ms / p99 232 ms, and the previous version (one long read batch per
  iteration, 48 read connections) fails between 8% and 12%:

  | loss | before: p95 / p99 | now: p95 / p99 |
  |---:|---:|---:|
  | 0% | 5.6 / 6.3 ms | 5.0 / 6.6 ms |
  | 5% | 216 / 232 ms | 210 / 216 ms |
  | 8% | 489 / 888 ms | 214 / 223 ms |
  | 12% | 2,873 / 4,029 ms | 218 / 425 ms |
  | 15% | | 221 / 621 ms |
  | 18% | | 417 / 828 ms |
  | 20% | | 443 / 1,052 ms (fails) |

  What `ss` showed on the Postgres side at 8%: every busy read connection at a congestion
  window of 1 or 2 packets with 50 to 65 KB of reply not sent yet, waiting 200 ms or more
  whenever a retransmitted packet was lost again. What changed:
  - Small read batches on many connections (above): a reply is a few packets, sent at
    once even at the minimum window, and a hole holds one or two requests instead of
    half an iteration's.
  - 448 read connections: a reply that needs Postgres' retransmission timer holds its
    connection for 200 ms or more. At 15% loss the same run with 192 read connections
    failed (p95 621 ms) and with 400 passed (p95 221 ms).
  - A connection that owes replies and has been silent for 3 ms gets a lone Sync
    (doubling up to 320 ms until bytes arrive). The ReadyForQuery that answers it is new
    data behind the lost packet, so the selective ACK makes Postgres resend it at once
    instead of after its timer. A Sync runs no statement (csharp-epoll-xeonfx does this
    after 50 ms). Write p99 at 8% loss: 438 ms to 246.
  - `TCP_QUICKACK` after a read that leaves replies outstanding, so a sender down to one
    packet per ACK does not wait for the 40 ms delayed-ACK timer.

  The cost without loss, same run: server CPU 14.9 to 17.9 µs per request, Postgres CPU
  28 to 45 µs, 1.19 to 1.66 packets out per request (in: 3.07 to 3.11).
- **gzip**: one hash probe per position for LZ77 matches, then one DEFLATE block with
  dynamic Huffman tables (length-limited, Moffat-Katajainen). Each loop builds the tables
  from one live body, every printable byte, match length and distance given a code, and
  codes the next 256 bodies with them; a body that needs a symbol without a code rebuilds
  them from itself. Each body is still built from its own query and compressed whole.
  Building tables for every body was ~40% of the encoder's time, and the 4-core score ran
  at 68% app CPU, more than the entries above it, so the tables are cached: the feed
  under `mload` went from 747 B in 6.5 µs to 756 B in 4.3 µs, and a simulated stream of
  mload feeds came out 2% larger in 38% less time. Earlier measurements, with per-body
  tables:

  | feed | fixed tables | own dynamic tables | stdlib level 1 |
  |---|---:|---:|---:|
  | under `mload` (3,618 B) | 902 B, 3.4 µs | 747 B, 6.7 µs | 780 B, 7.8 µs |
  | seed data (5,303 B) | 2,306 B, 7.3 µs | 1,884 B, 12.7 µs | |

  Under load (20,000 users, two runs each) that is 772 to 690 bytes sent to clients per
  request (packet headers included) for 14.0 to 14.7 µs going to 16.1 to 16.2 µs of CPU.
  Only bodies of 1 KB or more are compressed (in practice `/feed`).
- **JSON**: post objects are written straight from the row; `created_at` is formatted
  from the microsecond count
  (civil-from-days), truncated to milliseconds. Strings escape
  quotes, backslashes and control characters; non-ASCII stays UTF-8.
- **Validation**: unchanged from the previous version: HS256 signature, required numeric
  `exp` and payload types checked on every request, strict JSON request bodies, the
  SPEC's id and body rules. A panic in validation answers 500 for that request only.
- Reference SQL, one statement per request. No response, query-result or JWT caching.
- `start.sh` only starts the binary: no memory limit is set, and nothing outside the
  submission's folder is read (the heap stays small: 30 MB at 200,000 connections).

## Run

On the clean Ubuntu 24.04 x86_64 benchmark box, run `install.sh` as root once.
It installs Go 1.27.3 from the official archive after checking its SHA-256.
`build.sh` runs as the normal user; `start.sh` runs in the foreground.

```bash
bash submissions/go-epoll-trungdlp/build.sh
DATABASE_URL=postgres://feed:password@localhost:5432/feed \
JWT_SECRET=million-users-challenge HOST=0.0.0.0 PORT=3000 EXTRA_PORT=3001 \
  bash submissions/go-epoll-trungdlp/start.sh
```

Only `DATABASE_URL` and `JWT_SECRET` are required. Defaults: `HOST=127.0.0.1`
(IPv4), `PORT=3000`; `EXTRA_PORT` enables the second listener. The Postgres link must be
plain TCP (the box has `ssl = off`).

## Validation

```bash
cd submissions/go-epoll-trungdlp
go test -race ./...        # parser, wire format, JSON, routing and validation
GOOS=linux go vet ./...
cd ../..
bash seed/make-seed.sh
bash test/run.sh submissions/go-epoll-trungdlp   # on Linux
```

`cmd/checker` opens many keep-alive connections and checks that every `GET /posts/:id`
response carries the requested id, in order, with pipelined requests, requests split
across writes, and connections closed while their statement is in flight:
`go run ./cmd/checker -addr 127.0.0.1:3000 -conns 1000 -d 40s`. Latest run: 4.8M
responses, 0 mismatches, 0 errors, 187k mid-request closes, 21k slow readers (4 KB
receive buffer, 100 pipelined requests). With every Postgres backend terminated twice
during a run, in-flight requests get 500, the connections come back within a second,
and no request is left unanswered. A `-tags debug` build logs each loop's state on
SIGUSR1.

42/42 official checks pass for linux/arm64 and for linux/amd64 (x86_64 emulation on an
arm64 Mac), each in a container against Postgres 18.

## Measured locally

Docker on an Apple M4 Pro, linux/arm64 containers on one bridge network: server on 2
pinned vCPUs, `bench/mload` on 6, Postgres 18 on 6. `mload -gzip -think-scale 0.1`
(the challenge's request rate per user times 25, so fewer connections reach the same
request rate), 30 s ramp, 90 to 120 s hold. Server CPU per request is from its cgroup.

| Build | Users passing | CPU per request | p95 / p99 at 30k users | RSS at 30k users |
| --- | ---: | ---: | ---: | ---: |
| nbhttp + `pgxpool` | 30,000 | 48 µs | 164 / 286 ms | 179 MB |
| nbhttp + pipelined `pgconn` | 70,000 | 32 µs | 13 / 20 ms | 154 MB |
| own epoll loops | 80,000 | 16 µs | 7.5 / 8.3 ms | 34 MB |
| + own gzip encoder | 80,000 | 13 µs | 7.3 / 8.0 ms | 32 MB |
| + own timestamp formatting | 80,000 | 12 µs | 7.2 / 7.9 ms | 32 MB |

- The current build at 80,000 users: 73k req/s, 13 µs per request, p95 25 ms, p99 74 ms,
  0 errors.
  At 90,000 users the load generator was at 69% of its CPUs while the server used about
  two thirds of its 2: the local setup set that ceiling before the server did.
- 24 read + 8 write connections per loop instead of 3 + 1: 18 µs per request and 50% more
  Postgres CPU (smaller batches). No batching window: 21 µs per request; 4 ms: 17 µs.
- At the per-loop rate of the challenge's 8-core runs (~7k req/s per loop: 70,000 users,
  `-think-scale 0.5`): 15.5 µs per request, p95 6.0 ms, p99 7.6 ms.
- **200,000 connections** (two load generators of 100,000 users, `-think-scale 2.5`,
  120 s ramp, 60 s hold): 6.2k req/s, p95 9.0 ms, p99 10.3 ms, max 26 ms, 0 errors.
  Server RSS 30 MB with 200,020 descriptors open, 0.17 cores. The server's listen
  queues never overflowed (`ListenOverflows=0`, `ListenDrops=0`). A 60 s ramp failed
  with 2 to 4 s connect times, but only while both load generators were at 100% of
  their CPUs; the server's counters showed no drops. (The Docker VM's conntrack table,
  262,144 entries, kept this test under that size.)
- The other runs use far fewer connections than the challenge (tens of thousands, not
  millions), so they show CPU per request, not the memory of millions of idle sockets.
