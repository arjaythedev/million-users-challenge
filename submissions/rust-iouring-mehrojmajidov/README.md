# rust-iouring: Rust on io_uring, its own HTTP and Postgres wire client

- **Language:** Rust 1.99.1, from the official archives (SHA-256 pinned in `install.sh`), built with LTO and
  `-C target-cpu=native`.
- **Framework / IO:** none. io_uring through its three system calls, with the rings mapped by hand (`src/uring.rs`;
  no liburing, no crate): multishot accept, multishot receive into provided buffers, sends, and the Postgres
  sockets on the same ring. One `io_uring_enter` per pass. Where a ring cannot be made the same loop runs on epoll.
- **Dependencies:** none. `Cargo.lock` lists only this package, and the build runs offline.
- **Driver:** none. The Postgres v3 wire protocol is spoken from `src/pg.rs` and `src/db.rs`: SCRAM-SHA-256
  login, each statement parsed once per connection, one Bind/Execute per request.
- **Compression:** its own gzip encoder (`src/gzip.rs`).
- **Front:** direct, on `PORT` and `EXTRA_PORT`.
- **Postgres connections:** 80 in all on 4, 8 and 16 cores, 78 on 2 (48 for reads and 32 for writes, split over
  the loops).

This is `rust-epoll-mehrojmajidov` (#120) with the loop's I/O moved onto io_uring, as a second entry so the two
can be compared on the rig. Requests are parsed, batched, sent to Postgres and answered by the same code; what
changed is how bytes get in and out. On epoll a request costs a `recv` and a `writev`, plus the calls for
Postgres. Here the kernel receives and sends for the loop, and the loop enters the kernel once per pass. On two
CPUs it also runs three loops and longer passes than #120, which follows from what the measurements showed about
the box's card.

## What decides the score, and what this server does about it

Every top entry stops near 1.5 million users on 8 and on 4 cores with the box's CPU half idle (issue #62), and on
2 cores the entries that tie on 4 spread from 895,000 to 1,250,000. On a rented VM set up with
`box/setup-app-box.sh` (one card queue, RPS over the CPUs; "Measured" below), the two looked like one thing:

- **One CPU does the card's work.** The CPU that takes the single queue's interrupts does two to three times the
  packet work of any other. It is the busiest CPU by far, and it fills up while the box is half idle on average.
- **Its largest single cost is a lock.** It frees every transmitted packet under the transmit queue's lock
  (`virtnet_poll_tx`), which every CPU needs in order to send. With a server loop on each of four CPUs, about a
  fifth of that CPU's busy time was spent waiting for it.
- **What lands on that CPU decides how far a server gets.** On 2 CPUs the four servers compared below used
  within 8% of each other's CPU in total, but the card's CPU was 56% to 66% busy, in the order of their official
  2-core scores.

That VM is not the benchmark box, so this is evidence and not proof. What this server does about it, beyond taking
in as few packets and bytes per request as the protocol allows (unchanged from #120):

1. **No system call per request** (io_uring): 4 to 9% less CPU for the same traffic.
2. **Three loops on two CPUs**, with 4 ms passes: the scheduler can put two of them on the CPU that is not the
   card's, and Postgres gets half as many batches.
3. **One loop per CPU with 2 ms passes from three CPUs up.** Fewer loops left the card's CPU a quarter less busy
   on that VM (table below), but on the benchmark box the entries that ran fewer loops than cores scored lower
   (#90, #109, #111), and the box's word counts. `FEED_LOOPS=2 FEED_PASS_US=4000` gives the other layout.

## Design

**Loops.** One per CPU, except on two CPUs, where there are three. The 16-, 8-, 4- and 2-core runs get 16, 8, 4
and 3 loops. The CPUs are counted from the affinity mask (`sched_getaffinity`), and no thread is tied to a CPU:
the scheduler places them. Each loop has its own ring, its own `SO_REUSEPORT` listeners on both ports, its own
clients and its own Postgres connections. Loops share nothing and take no locks.

- Two loops on two CPUs would each be tied to a CPU, one of them the card's. With three, the scheduler gives the
  card's CPU one and the other CPU two: the busier CPU at 800,000 users went from 61% to 57%.
- On four CPUs two loops measured better than four on the VM: the card's CPU was 43% busy at 950,000 users
  instead of 53%. This entry does not ship that. A loop then takes four times the requests per pass, and at the
  box's ceiling (44,000 requests a second) that is about 90 a pass: more than one batch, and a reply burst four
  times the size. Three entries ran fewer loops than cores on the box and each scored lower than with one per
  core (#90: 1,420,000 to 1,260,000 on 8 cores; #109; #111).

**The ring.** Made by the loop's own thread with `SINGLE_ISSUER` and `DEFER_TASKRUN`: only that thread submits,
and completions are worked off only when it asks for them, so the kernel never interrupts a loop, or another core,
to deliver one. On it:
- a multishot accept per listener;
- a multishot receive per client into the ring's provided buffers (4,096 of 2 KB per loop). A buffer is taken
  only when bytes arrive, the request is parsed in place, and the buffer goes back before the next completion is
  looked at, so an idle connection holds none. If a pass brings more arrivals than there are buffers, the
  connections left without one wait in a queue and are armed again as buffers come back, in order;
- one send per response, its bytes kept in a pooled buffer until the completion arrives;
- for each Postgres connection one receive straight into its reply buffer, and one send per batch.

**A pass.** A loop works through the completions that are in, queues this pass's reads for Postgres as one batch
and its writes as one group, and then makes one `io_uring_enter` that submits everything queued (responses, the
Postgres batch, receives to arm) and waits out the rest of the pass: 2 ms, or 4 ms on one and two CPUs. The
completions that are in by then are the next pass. The kernel's work inside that call counts towards the pass (a
running average is taken off the wait), so the cadence is the same as on epoll. The wait ends early once 4,096
completions are in, so the cadence does not cap throughput, and a pass in which nothing came in is followed by a
wait for the first completion, so an idle loop does not tick. The cost of the cadence is about two passes of
latency per request.

**Clients.** A connection's state is a 16-byte slot indexed by its descriptor; buffers exist only while a request
is split across receives or a response is in flight. While a request is at Postgres, or a response is on its way
out, later requests on that connection wait (up to 80 KB), so responses stay in order and a client that does not
read cannot make the server buffer its answers. A new connection's first request is read when the connection is
accepted (the listener hands it over with the request already in), so it is served in the same pass. A client that
closes its side still gets the answers to what it sent.

**Closing.** A descriptor is closed only after the ring has reported the end of its receive and of any send in
flight (the socket is shut down first, which completes both). Its number therefore cannot be handed out again
while a completion for its previous life is still to come. A dropped Postgres connection likewise reconnects only
after its receive and send have reported back, because the kernel may use their buffers until then.

**Without io_uring.** If the kernel refuses the ring at start-up (containers with Docker's default seccomp
profile do, and so may CI), the loops run on epoll: one `recv` per request, one `writev` per response, the same
passes. The start-up line says which one runs. Both paths pass the same tests.

**Reads.** A pass's reads go to the read connection with the fewest replies outstanding, as Bind/Execute pairs
under one Sync, 64 at most; more go to the next connection. A read connection's buffer (256 KB) holds the whole
reply to a full batch. No Describe is sent, so a reply is BindComplete, DataRows and the end of the statement
and nothing else. A read's Execute asks for as many rows as it can have (20 for a feed, one for a post), so that
end is the 5-byte PortalSuspended instead of a CommandComplete of 14 or 15 bytes; the SQL is the SPEC's either
way. Each read is answered as soon as its own end arrives.
The JSON is written straight from the DataRow bytes.

**Writes.** Writes use connections of their own, so a commit never holds up a read. A pass's writes form one
group under one Sync (128 at most): one implicit transaction, one commit. Every write of the group is answered
only when that Sync's ReadyForQuery arrives, which Postgres sends after the commit. A write connection carries one
group at a time; while its commit is out, new writes wait and leave together as the next group. If a write fails
(a like on a missing post), Postgres rolls the group back: the failing write gets its own answer (404), and the
others are sent again, each alone between two Syncs, so each commits or fails by itself.

**The build.** A plain release build. A profile-guided one exists and is off, because it measured no faster
("Measured"): with `FEED_PGO=1` for both scripts, `install.sh` adds `llvm-profdata` and `build.sh` builds twice.
The first build is instrumented and includes `src/train.rs`: it runs the server on loopback for five seconds
against a stand-in for Postgres that answers the five statements with rows shaped like the load test's, while
clients send the load test's mix of requests (about 200,000 of them on the desktop below). The second build is
the one that serves; the compiler lays it out by what the first one executed, and it does not contain the
training code. If a step fails, or if the kernel refuses io_uring where the build runs (a profile of the epoll
fallback would mark the ring's code as unused), the build is a plain one.

**Lost packets and lost connections.**
- A connection that owes replies and has been silent for 50 ms gets a lone Sync, then again with doubling gaps
  (up to 800 ms). A Sync is not a query. If a segment was lost, the extra traffic lets TCP notice before its
  200 ms retransmit timer would.
- After 10 s of silence the connection is declared lost. Its reads are sent again on another connection (at most
  3 times per request); its writes answer 500, because a commit may have run and a write is never sent twice.
- Reconnecting starts at once. While no connection of the needed kind is up, requests wait up to 2 s for one
  before they are refused.

## Optimizations, and why

| What | Why |
|---|---|
| Loops that share nothing, `SO_REUSEPORT` per loop | no locks and no hand-off between threads on the request path |
| Three loops on two CPUs | two would each be tied to a CPU, one of them the card's; with three the scheduler puts two on the other CPU |
| io_uring with multishot accept and receive, sends on the ring | no system call per request: the loop enters the kernel once per pass, about 0.02 times per request at 13,000 requests a second per loop |
| `SINGLE_ISSUER` and `DEFER_TASKRUN` | completions are worked off when the loop asks, in one batch per pass, not by interrupting it |
| Provided buffers through a buffer ring, 4,096 of 2 KB per loop | an idle connection holds no buffer, and a buffer goes back to the kernel without a request |
| Passes of 2 ms (4 ms on one and two CPUs) | one wake-up serves everything that arrived meanwhile; a pass's reads share a Sync and its writes a commit. Longer on two CPUs, where CPU runs out first: half as many batches |
| A pass's reads on one connection under one Sync | one packet to Postgres and one wake-up of one backend per batch; replies come back in full packets |
| No Describe, so no RowDescription | a reply carries the rows and nothing about their columns, which libpq asks for with every query |
| `id` and `like_count` as text, `created_at` binary | per column, the shorter format: a few digits against 8 and 4 bytes, 8 bytes against 29. Postgres replies are most of the bytes coming into the box |
| Separate write connections, group commit | a commit only delays other writes; one WAL flush for a pass's writes |
| `TCP_QUICKACK` off on the listener after `listen()`, which accepted sockets inherit; set again after a response slower than 30 ms (through the ring, so not a system call either) | the kernel would ACK a request at once in a packet of its own; this way the response carries the ACK. Set on the listener it already holds for a connection's first request, which arrives before the server sees the connection: one packet fewer per new connection, and no system call per accept |
| `TCP_DEFER_ACCEPT`, read straight after `accept` | a new connection arrives with its first request, so one wake-up serves both |
| `TCP_THIN_LINEAR_TIMEOUTS` on client and Postgres sockets | with few packets in flight a segment lost twice waits two retransmit timeouts, not three |
| Loop threads in `SCHED_IDLE` | on a shared core the kernel's receive work (`ksoftirqd`) runs first, so the card's single queue does not overflow behind a busy loop |
| Lone Sync on a silent Postgres connection | earlier loss detection than the retransmit timer |
| gzip for bodies of 1 KB or more (in practice the feed) | a feed of the load test's posts is about 3.7 KB, three segments; compressed it is about 1.1 KB, one segment |
| Own gzip encoder: one fixed-Huffman block, greedy matches from a 4-byte hash table (16 KB) that is never cleared, a search at every other byte in runs without a match, the distance code computed instead of looked up, CRC-32 by carry-less multiplication | about 3 µs per feed here, measured over feeds that all differ (one feed repeated lets the processor learn its branches and reads 1.9 µs); at this size a general library's setup costs more than the work. The table size was measured inside the running server: 4 KB to 64 KB cost the same, 128 KB costs 3% more, so it is the size that fits a first-level cache twice over |
| 16-byte connection slot, no per-connection buffers | 950,000 open connections cost 58 MB of the server's own memory when measured with two loops, 16 MB of it their receive buffers (8 MB a loop) |
| Descriptors closed only after the ring's last completion for them | a number cannot be handed out again while a completion for its previous life is still to come |
| A feed's date text kept from row to row, digits two at a time | the 20 rows of a feed are nearly all of one day, and the calendar arithmetic was most of a timestamp: 91 ns per feed against 445. Kept for one response only |
| HMAC key blocks absorbed once | a token check costs only the hashing of the token itself |
| A row of a response is written into one reservation; its strings are copied 16 bytes per step (SSE2) and checked for characters to escape on the way, likewise the search for the end of a request head | a feed's 40 strings are 1.9 KB, and nearly all of them need no escaping: 200 ns per feed against 1,250 ns byte by byte. With the encoder's changes, 4% less of the server's own CPU time per request |

## Rules notes

- **Rule 3.** The five statements are exactly those of `SPEC.md` (the feed with the literal `LIMIT 20`), plus
  `SELECT 1` for `/health`: one per request, prepared and pipelined. The result formats are a protocol choice; the
  SQL is the same. A read is sent a second time only after Postgres skipped it (an earlier statement of its batch
  failed) or its connection was lost, never while its first copy may still run. There is no hedging.
- **Rule 5.** At most 80 connections to Postgres; whatever is configured, the code caps the total at 512.
- **Rule 7.** Nothing is cached across requests: every response is built from a query run while serving it. The
  gzip table holds positions in the current input only; entries from earlier calls read as empty.
- **Rule 8.** A write is answered only at the ReadyForQuery of the Sync that commits it. `synchronous_commit` is
  not touched.
- **Rule 9.** Every request's signature and `exp` are checked; a token without `exp` is refused. No verification
  is remembered.
- **Rule 12.** Built from source by `build.sh` with the toolchain `install.sh` pins. No prebuilt binaries, and
  no profile is shipped: the optional profile-guided build makes its own on the machine it builds on.
- **Rule 13.** No CPU pinning and no option for it. No file outside the folder is read (`sched_getaffinity` counts
  the CPUs). `SCHED_IDLE` is the policy of this process's own threads; the TCP options are per socket; a ring
  belongs to the thread that made it. No kernel-side polling thread (`SQPOLL`) is used.

## Measured

**On one desktop** (Intel i5-14400, Linux 7.2), Docker laid out like the benchmark: the server in a container on 4
CPUs (with io_uring allowed; Docker's default seccomp profile forbids it), Postgres 18.6 with the planner settings
of `box/postgresql.conf` on 6 others, `bench/mload` with `-gzip` on the remaining 6. Clients and Postgres are on
separate networks and segmentation offload is off, so the server's interface counters are TCP segments as a wire
carries them. CPU is the container's cgroup time (user + system). Three rounds each, in turn; the middle column is
the same binary with `FEED_IO=epoll`, so the only difference to the first is the ring. Both ran two loops with
4 ms passes (`FEED_LOOPS=2 FEED_PASS_US=4000`), the layout this entry was first written with.

| 40,000 users, `-think-scale 0.1`, about 42,000 req/s | on io_uring | same binary on epoll | c-epoll |
|---|---:|---:|---:|
| Packets into the server per request | 2.98 to 2.99 | 2.98 to 3.04 | 3.00 to 3.03 |
| Bytes into the server per request | 1,601 to 1,604 | 1,603 to 1,624 | 1,605 to 1,620 |
| Response bytes out per request | 648 to 649 | 650 to 663 | 697 to 705 |
| Server CPU per request | 6.3 to 6.7 µs | 6.8 to 9.6 µs | 7.7 to 9.9 µs |
| p50 / p95 over the run | 7.3 / 12.3 ms | 6.8 / 8.3 ms once; p95 57 and 272 ms twice | 3.3 / 4.3 ms |
| Server's resident memory | 27 to 29 MB | 10 to 14 MB | 9 to 11 MB |

What goes over the wire is unchanged from #120. The 4 ms passes show in the median, and the receive buffers (8 MB
a loop) in the memory. On epoll, two loops that each carry 21,000 req/s through a system call per receive and per
send had long tails in two of three rounds here; on the ring they did not.

The two layouts at the same load, which is close to the rate at the box's 4-core ceiling. Two rounds each on a
quiet machine; rounds taken while another job ran on the desktop were slower for both and are left out.

| 40,000 users, `-think-scale 0.1`, about 42,000 req/s, 4 CPUs | 4 loops, 2 ms (the default) | 2 loops, 4 ms |
|---|---:|---:|
| p50 / p95 / p99 over the run | 2.9 to 3.3 / 4.1 to 4.2 / 4.5 to 4.6 ms | 7.3 / 12.7 to 13.1 / 16.1 to 18.0 ms |
| Server CPU per request | 6.8 to 7.0 µs | 7.1 to 7.2 µs |
| Packets to Postgres per request | 0.13 to 0.15 | 0.08 |
| Packets into the server per request | 3.01 | 2.99 |
| Server's resident memory | 45 to 54 MB | 29 to 30 MB |

**On a rented VM laid out like the benchmark** (Oracle Cloud): an app box with 4 vCPUs of a Xeon 8358 on Ubuntu's
6.8.0-149 kernel, set up by `box/setup-app-box.sh` with its card cut to one queue; a database box with 4 vCPUs set
up by `box/setup-db-box.sh`; one 12-core load generator running `bench/mload` with the scoring flags and ramp.
CPUs were taken offline for the smaller runs, holds lasted two minutes, and every run had 0.000% errors. Busy
means not idle, from `/proc/stat`; "the card's CPU" is the one that takes the card's interrupts. This generator
stops near 950,000 users (every server's p99 passes one second there) and adds latency of its own about every
35 seconds, so latency near the top does not separate servers on this rig; CPU does. Single runs: repeats
differed by 2 to 3 points.

| 4 CPUs, 950,000 users, 27,100 req/s | this entry with `FEED_LOOPS=2 FEED_PASS_US=4000` | c-ammarserver (#116) | c-epoll |
|---|---:|---:|---:|
| The card's CPU, busy | 39.3% | 52.7% | 55.3% |
| of it server work / packet work | 8.5 / 30.5 | 14.5 / 38.1 | 12.3 / 42.7 |
| All four CPUs, busy | 35.8% | 41.7% | 44.6% |
| Context switches per second | 1,171 | 3,407 | 4,006 |

| 2 CPUs (two threads of one core) | this entry | #120's build (epoll, 2 loops, 2 ms) | c-ammarserver (#116) | c-epoll |
|---|---:|---:|---:|---:|
| 800,000 users: the card's CPU | 55.6% | 64.0% | 56.3% | 66.2% |
| 800,000 users: both CPUs | 50.7% | 54.3% | 51.4% | 55.6% |
| 800,000 users: p95 / p99 | 200 / 672 ms | 212 / 659 ms | 214 / 679 ms | 355 / 812 ms |
| 900,000 users: the card's CPU | 62.3% | | 66.4% | |

| This entry on 4 CPUs, 950,000 users | The card's CPU | All four CPUs |
|---|---:|---:|
| 8 loops, 2 ms passes | 55.3% | 44.6% |
| 4 loops, 2 ms (one per CPU, the default) | 52.5% | 41.8% |
| 4 loops, 4 ms | 50.2% | 39.0% |
| 2 loops, 2 ms | 42.8% | 37.0% |
| 2 loops, 4 ms (two runs) | 40.9% and 39.3% | 37.0% and 35.8% |

- **Where the card's CPU spends it.** A profile of that CPU with four loops on four CPUs: 9.8% of its samples, a
  fifth of its busy time, in the spinlock slow path called from `virtnet_poll_tx`.
- **On two CPUs** three loops put the card's CPU at 56.6% and two loops at 60.6% (both with 2 ms passes); four
  loops did no better than three (56.7%).
- **io_uring alone**, with the same loops and passes as #120's build: 60.1% against 66.1% busy on one CPU at
  650,000 users, 51.9% against 54.3% on two at 800,000.
- **What is not shown**: the ceiling. This rig cannot load a server to it, so it cannot say what two loops would
  hold on four cores. The benchmark box's results for entries with fewer loops than cores say less, not more,
  which is why the default is a loop per core.

- **With the CPU as the limit** (the server held to 30% of one CPU, so that it gives out before Postgres and
  the generators do; 40,000 to 50,000 users at `-think-scale 0.1`, two rounds): this entry reached 49,000 req/s
  at 4.9 to 5.0 µs of its own CPU per request, c-epoll 37,000 req/s at 8.1 µs. On two CPUs under the same cap,
  with three loops, it reached 42,500 to 45,800 req/s.
- **The profile-guided build** made no difference that this desktop could show: CPU per request was the same
  within the run-to-run spread over seven alternating rounds, the gzip encoder ran at the same speed and request
  parsing about 10% faster. On one CPU capped at 30% it carried the same load as the plain build (47,000 req/s
  at most, 5.1 to 5.5 µs per request). So it is off by default.
- **A frozen server** (paused for 1.5 s in a 60 s run at 36,000 req/s, so that a backlog has to be worked off):
  1.6 to 1.7% of replies took over a second with the defaults, 1.4% and 3.1% with `FEED_READS_IN_FLIGHT=256`.
  No gain to show, which is why that setting is off.
- **System calls.** Over loopback at 27,000 req/s on two loops with 2 ms passes, the server made 1.25
  `write`-family calls per request on epoll and none on the ring (`/proc/<pid>/io`; receives are not counted
  there), with the same median latency (4.1 and 4.2 ms).
- **Every Postgres backend terminated** in the middle of a run (3,000 users, 80 connections at once, about ten
  runs per path): all 80 connections came back each time, no read failed, and the only failures were writes in
  flight at that moment (0 to 7 a run on the ring, 0 to 6 on epoll), which answer 500 because a write is never
  sent twice.
- **A pool of two receive buffers** per loop instead of 4,096: every test below still passes, and 17,000 req/s are
  served with p95 8 ms. Running out of buffers slows a loop down; it does not stall it.
- **Connections that leave in every way a client can** (after the answer, before it, in the middle of a request,
  half-closed, never reading, a thousand at once): all answered where an answer was due, and no descriptor left
  open afterwards, on both paths.
- **`test/run.sh`:** `passed=42 failed=0` in a clean `ubuntu:24.04` container after `install.sh`, on epoll (what
  the container allows by default) and on io_uring (with seccomp off).
- The entry's own tests (`cargo test`, 39 tests) cover the ring (multishot receive into provided buffers both
  ways of returning them, sends larger than a socket buffer, a full submission queue, a socket option set through
  the ring), the token rules, JSON reading and escaping, timestamps, HTTP framing and routes, the Postgres
  messages, and gzip round trips through the `gzip` tool.

## Configuration

Only the variables of `SPEC.md` are needed. These are optional and were used for the measurements above:

| Variable | Default | |
|---|---|---|
| `FEED_LOOPS` | one per CPU; 3 on 2 CPUs | loop threads |
| `FEED_PG_READERS`, `FEED_PG_WRITERS` | 48 and 32 divided by the loops, at least 2 | Postgres connections per loop |
| `FEED_PASS_US` | 2000; 4000 on 1 and 2 CPUs | length of a pass in microseconds (0: no pause) |
| `FEED_SCHED_IDLE` | 1 | 0 leaves the loop threads in the default scheduling class |
| `FEED_IO` | io_uring where the kernel allows it | `epoll` or `uring` forces one (`uring` fails at start-up if it cannot be had) |
| `FEED_RING_BUFFERS` | 4096 | receive buffers per loop's ring, a power of two |
| `FEED_BUFFER_RING` | 1 | 0 hands receive buffers back with one request each, as on a kernel without buffer rings |
| `FEED_READS_IN_FLIGHT` | 0 (no limit) | an experiment, off: at most this many reads per loop at Postgres; the rest wait in the loop, and once the oldest has waited `FEED_BACKLOG_AGE_US` (10000) the newest go first |
| `FEED_PGO` (for `install.sh` and `build.sh`) | 0 | 1 makes the profile-guided build |

## The code

- `src/main.rs`: configuration and startup. `src/sys.rs`: the libc calls and constants.
- `src/uring.rs`: io_uring (set-up, the two queues, the buffer ring, the requests the loop uses).
- `src/reactor.rs`: the loop and its clients (accept, receive, respond, close), on the ring and on epoll.
  `src/db.rs`: the Postgres side of a loop (batches, write groups, the reply stream, lost connections).
- `src/pg.rs`: login, prepared statements and request messages. `src/http.rs`: request parsing, routes, heads.
- `src/json.rs`, `src/jwt.rs`, `src/crypto.rs` (SHA-256, HMAC, PBKDF2, base64), `src/gzip.rs`, `src/time.rs`.
- `src/train.rs`: the training run of the optional profile-guided build (the stand-in for Postgres and its
  clients); not in the binary that serves.

## Running

```bash
sudo ./install.sh   # gcc as the linker, Rust 1.99.1 into /opt/feed-rust
./build.sh          # target/release/feed
DATABASE_URL=postgres://user:pass@host:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 ./start.sh
```
