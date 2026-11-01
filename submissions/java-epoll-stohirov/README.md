# Java + its own epoll event loops + its own Postgres client

| | |
|---|---|
| Language | Java 25: Eclipse Temurin `jdk-25.0.4.1+1`, official tarball, sha256-pinned in `install.sh` |
| Framework | None. A hand-written HTTP/1.1 server on Linux `epoll` (or `io_uring`), called through FFM (`java.lang.foreign`), one event loop per core |
| Postgres driver | None. A small v3 wire-protocol client on the same event loops: prepared statements, pipelining, binary results, SCRAM-SHA-256 |
| JSON | Written byte by byte into the response; a small strict parser reads request bodies and JWTs |
| Dependencies | Zero. `build.sh` needs only the JDK |

## Running it

```bash
sudo bash install.sh   # Temurin 25 to /opt/jdk
bash build.sh          # src/ -> build/server.jar
DATABASE_URL=postgres://... JWT_SECRET=... HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

Linux only (it calls `epoll` directly). Optional env: `LOOPS` (event loop threads, default: the cores the JVM
sees), `DB_POOL` (read connections to Postgres in all, default 64, split over the loops), `DB_WRITERS` (write
connections per loop, default 4), `PACK_MAX` (most reads behind one `Sync`, default 64), `GZIP_LEVEL` (0 turns gzip
off), `GZIP_MIN` (smallest body to compress, default 1024 bytes), `IO` (`epoll`, the default, or
`uring`) and `BATCH_MIN` / `BATCH_MICROS` (wake-up batching, default 128 events / 4000 µs; `BATCH_MICROS=0` turns it off).

## What decides the score

CPU doesn't. On the rig, every top entry stops at about 56-58k req/s with the server's CPU far from full, and a p95 of
~200 ms, TCP's minimum retransmission timeout, says packets are being lost on the way (see issue #62). So this server is
built to send **as few packets as possible**, and to lose **as little time as possible** when one goes missing.

## Design

**Event loops, nothing shared.** `LOOPS` threads, each with its own epoll set, its own listening socket on both
ports (`SO_REUSEPORT`, so the kernel spreads new connections over the loops), its own clients and its own Postgres
connections. No locks, no queues between threads.

**Postgres on the event loop.** Each loop has `DB_POOL / LOOPS` read connections and `DB_WRITERS` write connections, all
non-blocking sockets in the same epoll set as the clients. A request that needs the database writes its
Bind/Execute/Sync into a connection's buffer, and the client waits without holding a thread. Every buffer
is flushed once per loop iteration, so everything that arrived together goes to Postgres in one `write`.

- **The SPEC's SQL, prepared once per connection** (rule 3). Per column, whichever result format is smaller:
  `created_at` is binary (8 bytes instead of 29), microseconds since 2000, cut to milliseconds and formatted without
  allocating. `id` and `like_count` are text (a new post's count is 1 byte instead of 4), which also goes straight
  into the JSON with no number to format.
- **Reads share a connection and a Sync.** All the reads of a loop pass go to one connection, the one with the
  fewest waiting, behind one Sync. That is one packet to Postgres, and since Postgres only flushes at a Sync or a
  full 8 KB buffer, far fewer packets back. Each read still runs its own statement and is answered at its own
  CommandComplete. If one fails, Postgres skips the rest up to the Sync: the failed one gets a 500 and the skipped
  ones are sent again.
- **Group commit for writes** (rule 8 allows it). While a transaction is in flight, new likes and posts queue up.
  When it commits, the whole queue goes out as the next transaction: one Sync, one commit, one WAL flush. Each
  response is sent only after its transaction has committed.
- **A lost packet doesn't stall the writes behind it.** With one write connection, a single lost packet held every
  queued write on that loop for the 200 ms retransmission timeout. Now, if a commit hasn't come back within 10 ms,
  the queue goes out on another of the loop's write connections, and only the batch that lost a packet waits. A
  healthy commit returns well inside that, so batches stay as large as before.
- **A failed batch is retried one by one.** A like on a missing post fails its foreign key (23503), and that
  rolls back the whole batch. The batch is then retried, each write in its own transaction, so each gets its
  own answer: 404 for the missing post, 201/200 for the rest.
- **One request per client waits on Postgres at a time.** Pipelined HTTP requests behind it wait in the
  connection's buffer, so responses keep their order.
- **Connection loss.** Everything waiting on a lost connection gets a 500 (`/health` gets 503). The loop
  reconnects on demand, at most one failed attempt per second, so a database outage doesn't stall it.

**Wake-up batching.** When a wake-up brings fewer than `BATCH_MIN` events, the loop waits up to `BATCH_MICROS`
before the next one, so later wake-ups carry more events, and more queries go to Postgres per `write`.
Responses are sent before the wait, so it delays only reading. Going from 2 to 4 ms halves the packets to Postgres
(0.14 to 0.07 per request) for a p95 of ~10 ms instead of ~6 ms (the limit is 500 ms). A larger flight also recovers
from loss faster: a lone lost packet waits at least 200 ms, one among several is repaired in about a round trip.

**Listening sockets.** `TCP_DEFER_ACCEPT`: a connection is accepted once its first request has arrived, so one
wake-up per new connection instead of two (a million of them during the ramp). `TCP_NODELAY` is set on the
listening socket and Linux copies it to accepted ones, so there is no `setsockopt` per connection.
`TCP_QUICKACK` is cleared on the listening socket after `listen()`, so a request's ACK rides on its response: 5.0 to
4.0 packets sent per new connection, measured. It is cleared again on a socket whose response took 25 ms or more,
since the delayed-ACK timer firing takes the socket out of that mode.

**io_uring (`IO=uring`, off by default).** Linux 6.8 features only (Ubuntu 24.04): multishot accept and
receive with a provided-buffer ring, `SEND` with `MSG_DONTWAIT` (it completes while it is submitted, so the
send arena is free right after), and one `io_uring_enter` per iteration that submits the sends and waits
for "`BATCH_MIN` completions or `BATCH_MICROS`". A closed connection's fd is closed only once nothing in the
ring refers to it, so a late completion can't hit a reused fd. If the kernel or a seccomp profile refuses
io_uring (Docker's default profile does), the loop uses epoll. Locally it was no cheaper than epoll (table
below). Syscalls cost more on x86 VMs with Spectre mitigations, though, so it is worth measuring on the real box.

**Memory per idle client.** A connection is an fd plus a small object. Read and write buffers are shared per loop.
A connection gets its own buffer only while it holds a partial request or a response the socket didn't
take yet.

**HTTP.** It parses the raw bytes of the request line, `Content-Length`, `Transfer-Encoding: chunked`,
`Authorization` and `Connection`. Status lines are precomputed, and each response goes out in one `write`.

**gzip, for the feed only.** A single post saves 15% at best and often grows, so only bodies of at least `GZIP_MIN`
bytes are compressed, which in practice means the feed. Under load the feed isn't the seed's: within moments it is 20
of the load test's own posts, which compress far better, 3,657 bytes to under 900. That is one packet instead of three.

It is the biggest piece of user CPU left, so the DEFLATE encoder is our own (`Gzip.java`), made for one small buffer
at a time: greedy LZ77 with a 4-byte hash over a 16k-entry table that is never cleared (entries carry a base that
moves on with every call, so old ones read as stale). If the fixed Huffman codes already fit in one packet (1,200
bytes), it uses them: building dynamic tables would save bytes but not a single packet. On the feed under load
(arm64): 3.9 µs for 870 bytes, against 5.9 µs for 723 with dynamic tables and 755 for `java.util.zip` at level 1.
A larger body gets one dynamic Huffman block with length-limited codes, or a fixed or stored block when that comes
out smaller. CRC32 is the JDK's (a CPU intrinsic). On the seed's feed (Temurin 25, Ubuntu 24.04, arm64, same JVM,
alternating): 10.1 µs for 1,923 bytes, where `java.util.zip` takes 13.1 µs for 1,972 at level 1 (level 6: 22 µs,
1,876 bytes); libdeflate, for reference, takes 8.7 µs for 1,901. Left to itself, C2 sometimes inlines the two hot
loops into their callers and compiles them worse (~12 µs), so `start.sh` keeps them apart with
`CompileCommand=dontinline`.

**JWT.** Verified in full on every request (rule 9): one keyed `Mac` per loop, the signature is base64url-encoded
straight from the digest and compared in constant time, HS256 only, and `exp` is required.

**JVM.** `ParallelGC` with a fixed, pre-touched 2 GB heap. Steady state allocates little: a `Conn` per
connection, and request bodies.

## Local measurements

These numbers are for comparing settings, not predictions for the benchmark boxes: there, the NIC and the kernel's
network processing (one RX queue, RPS) are in the path.

**Packets, CPU and loss.** Docker on an Apple Silicon laptop (arm64, one VM shared with other work): server, Postgres
and `mload` each in their own network namespace, so the kernel's counters split clients from Postgres. 2 loops, 20,000
users at `-think-scale 0.125` (~16,800 req/s, a loop's share at about 1.6M users on 8 cores), `-gzip`, every
configuration run at least twice, alternating. Segments repeat within 0.02. CPU moves by 10-25% from run to run
here, so only the larger CPU differences mean anything.
`c-epoll` is the leaderboard's first, run through the same harness at the same load:

| | Segments per request | to / from Postgres | Server µs/req | Postgres µs/req | p95 | p99 | p99, 0.5% loss | write p95, 0.5% loss |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| this entry at its scored commit | 5.22 | 0.79 / 1.43 | 19-25 | 46-62 | 5.6 ms | 6.7 ms | 159-174 ms | 177 ms |
| `c-epoll-unorig-username` | 4.36 | 0.19 / 1.17 | 16-17 | 31-34 | 8.4 ms | 10 ms | 11 ms | 6 ms |
| **this entry now** | **4.09** | **0.07 / 1.00** | **13-15** | **20-23** | 10 ms | 12.5 ms | **17 ms** | **11 ms** |

The client's share is the floor for all three: 3.00 (request, response, the client's ACK). Loss was injected with
`netem` on everything entering the app box.

**Correctness under failure.** 400 keep-alive connections, up to 4 pipelined requests each, feed, post, like and create,
with every response checked against its own request (post id, like `post_id`, the new post's body and author):
1.43M responses, 0 mismatched, while Postgres cancelled 110 running queries and terminated 17 backends. The process
stayed up. Each cancelled query cost one 500, and each terminated backend about 24, the requests it had in flight.

**Earlier measurements**, before the changes above. Docker on the same laptop: Postgres 18, `mload` and the server
share the machine, over loopback.

- **CPU per request**, mixed load at ~5k req/s, 4 loops: 32-34 µs without gzip, 36 µs with it (user + kernel
  time of the server process). Unpinned runs like this vary by up to ±25% from run to run: the VM's vCPUs land
  on the laptop's performance or efficiency cores. Pinned runs (`taskset -c 0`) repeat within ~1 µs.
- **Backend and batching**, one core (`taskset -c 0`, `LOOPS=1`), 40,000 users at `-think-scale 0.1`
  (~40k req/s), every configuration run twice, alternating, with the same results each time:

  | | user µs/req | kernel µs/req | p95 | p99 |
  |---|---|---|---|---|
  | epoll | 6 | 12 | 0.7 ms | 3.5 ms |
  | epoll + batching (default) | 6 | 6 | 8.6-9.2 ms | 11-13 ms |
  | io_uring | 6 | 13 | 0.75 ms | 2.8 ms |
  | io_uring + batching | 6 | 8 | 2.4 ms | 4.2 ms |

- **One core** (`taskset -c 0`, `LOOPS=1`, `DB_POOL=4`), `-think-scale 0.1 -gzip`, before wake-up batching:

  | Users | req/s | p95 | p99 | Errors |
  |---|---|---|---|---|
  | 15,000 | 14,963 | 0.40 ms | 0.59 ms | 0% |
  | 25,000 | 24,942 | 0.52 ms | 0.78 ms | 0% |
  | 35,000 | 34,895 | 0.66 ms | 6.5 ms | 0% |
  | 45,000 | 44,856 | 0.90 ms | 7.7 ms | 0% |
  | 55,000 | 54,745 | 5.1 ms | 19 ms | 0% |

- **Memory per connection**: 60,000 users at `-think-scale 2.5` (scoring think time) over 4 loopback IPs:
  0 errors. The live heap per connection is the 72-byte `Conn` plus its slot in the fd table (class
  histogram). An idle socket holds no kernel buffer pages.

## Not done yet

- Measuring these changes on the real boxes. At its previous commit this entry scored 1,000,000 / 1,310,000 / 1,260,000
  on 16 / 8 / 4 cores, p95 200 ms at a million. The local numbers above show fewer packets and a smaller cost per lost
  packet, not how far the ceiling moves.
- Tuning against those runs: loop count, readers per loop, pack size, batching, epoll vs io_uring, GC and heap size.

## License

MIT, under the repo's [license](../../LICENSE).
