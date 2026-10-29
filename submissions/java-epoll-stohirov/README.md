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
sees), `DB_POOL` (read connections to Postgres in all, default 64, split over the loops), `GZIP_LEVEL` (0 turns gzip
off), `GZIP_MIN` (smallest body to compress, default 1024 bytes), `IO` (`epoll`, the default, or
`uring`) and `BATCH_MIN` / `BATCH_MICROS` (wake-up batching, default 32 events / 2000 µs; `BATCH_MICROS=0` turns it off).

## Design

**Event loops, nothing shared.** `LOOPS` threads, each with its own epoll set, its own listening socket on both
ports (`SO_REUSEPORT`, so the kernel spreads new connections over the loops), its own clients and its own Postgres
connections. No locks, no queues between threads.

**Postgres on the event loop.** Each loop has `DB_POOL / LOOPS` read connections and one write connection, all
non-blocking sockets in the same epoll set as the clients. A request that needs the database writes its
Bind/Execute/Sync into a connection's buffer, and the client waits without holding a thread. Every buffer
is flushed once per loop iteration, so everything that arrived together goes to Postgres in one `write`.

- **The SPEC's SQL, prepared once per connection** (rule 3). Results are in binary format: ids and counts are
  integers, and `created_at` is microseconds since 2000, cut to milliseconds and formatted without allocating.
- **Pipelined reads.** A read goes to the reader with the fewest requests waiting. Each is its own unit (ending in
  Sync), so an error in one doesn't touch the others.
- **Group commit for writes** (rule 8 allows it). While a transaction is in flight on the loop's writer, new likes
  and posts queue up. When it commits, the whole queue goes out as the next transaction: one Sync, one commit,
  one WAL flush. Each response is sent only after its transaction has committed.
- **A failed batch is retried one by one.** A like on a missing post fails its foreign key (23503), and that
  rolls back the whole batch. The batch is then retried, each write in its own transaction, so each gets its
  own answer: 404 for the missing post, 201/200 for the rest.
- **One request per client waits on Postgres at a time.** Pipelined HTTP requests behind it wait in the
  connection's buffer, so responses keep their order.
- **Connection loss.** Everything waiting on a lost connection gets a 500 (`/health` gets 503). The loop
  reconnects on demand, at most one failed attempt per second, so a database outage doesn't stall it.

**Wake-up batching.** When a wake-up brings fewer than `BATCH_MIN` events, the loop waits up to `BATCH_MICROS`
before the next one, so later wake-ups carry more events, and more queries go to Postgres per `write`.
Responses are sent before the wait, so it delays only reading. On one busy core (~40k req/s, below) it cuts
kernel time per request in half, 12 to 6 µs, for a p99 of ~12 ms instead of ~3.5 ms (the limit is 1 s). On an
idle loop it only adds a timer wake-up, but such a loop has CPU to spare.

**Listening sockets.** `TCP_DEFER_ACCEPT`: a connection is accepted once its first request has arrived, so one
wake-up per new connection instead of two (a million of them during the ramp). `TCP_NODELAY` is set on the
listening socket and Linux copies it to accepted ones, so there is no `setsockopt` per connection.

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

**gzip, for the feed only.** The feed shrinks from 5,102 to 1,923 bytes, but a single post saves 15% at best and
often grows. So only bodies of at least `GZIP_MIN` bytes are compressed, which in practice means the feed. That is
~19k responses/s at a million users, ~0.8 Gbit/s down to ~0.3, and a feed fits in 2 packets instead of 4.

It is the biggest piece of user CPU left, so the DEFLATE encoder is our own (`Gzip.java`), made for one small buffer
at a time: greedy LZ77 with a 4-byte hash over a 16k-entry table that is never cleared (entries carry a base that
moves on with every call, so old ones read as stale), then one dynamic Huffman block with length-limited codes, or a
fixed or stored block when that comes out smaller. CRC32 is the JDK's (a CPU intrinsic). On the seed's feed (Temurin
25, Ubuntu 24.04, arm64, same JVM, alternating): 10.1 µs for 1,923 bytes, where `java.util.zip` takes 13.1 µs for
1,972 at level 1 (level 6: 22 µs, 1,876 bytes); libdeflate, for reference, takes 8.7 µs for 1,901. Left to itself,
C2 sometimes inlines the two hot loops into their callers and compiles them worse (~12 µs), so `start.sh` keeps them
apart with `CompileCommand=dontinline`.

**JWT.** Verified in full on every request (rule 9): one keyed `Mac` per loop, the signature is base64url-encoded
straight from the digest and compared in constant time, HS256 only, and `exp` is required.

**JVM.** `ParallelGC` with a fixed, pre-touched 2 GB heap. Steady state allocates little: a `Conn` per
connection, and request bodies.

## Local measurements

Docker on an Apple Silicon laptop (arm64, one VM): Postgres 18, `mload` and the server share the machine, over
loopback. These numbers are for comparing settings, not predictions for the benchmark boxes: there, the NIC and
the kernel's network processing (one RX queue, RPS) are in the path.

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

- Measuring on real boxes: 1M connections, the NIC path, then 8 and 4 cores.
- Tuning against those runs: loop count, readers per loop, pipelining depth, batching, epoll vs io_uring,
  GC and heap size.

## License

MIT, under the repo's [license](../../LICENSE).
