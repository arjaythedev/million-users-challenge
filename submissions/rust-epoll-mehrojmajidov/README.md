# rust-epoll: Rust, one epoll loop per core, its own HTTP and Postgres wire client

- **Language:** Rust 1.99.1, from the official archives (SHA-256 pinned in `install.sh`), built with LTO and
  `-C target-cpu=native`.
- **Framework / IO:** none. Raw epoll through hand-declared libc calls, no async runtime, hand-written HTTP/1.1.
- **Dependencies:** none. `Cargo.lock` lists only this package, and the build runs offline.
- **Driver:** none. The Postgres v3 wire protocol is spoken from `src/pg.rs` and `src/db.rs`: SCRAM-SHA-256
  login, each statement parsed once per connection, one Bind/Execute per request.
- **Compression:** its own gzip encoder (`src/gzip.rs`).
- **Front:** direct, on `PORT` and `EXTRA_PORT`.
- **Postgres connections:** 80 in all on 4, 8 and 16 cores (48 for reads, 32 for writes, split over the loops).

## What decides the score, and what this server does about it

On the benchmark boxes the top entries stop near 1.5 million users with the server's CPU half idle: the app box's
network gives out first (issue #62), and most of what comes into the box is Postgres replies. So the aim is to take
in as few packets and bytes per request as the protocol allows, and to lose as little time as possible when a
packet goes missing. CPU per request comes second.

## Design

**Loops.** One thread per CPU in the affinity mask (`sched_getaffinity`), so the 8- and 4-core runs get 8 and 4
loops. Each loop has its own `SO_REUSEPORT` listeners on both ports, its own clients and its own Postgres
connections. Loops share nothing and take no locks. A loop connects to Postgres before it opens its listeners, so
a new connection is never handed to a loop that cannot serve it yet; if the database stays away for ten seconds
the loop listens anyway and answers 503 until it is back.

**A pass.** A loop serves everything that is ready, sends this pass's reads to Postgres as one batch and its writes
as one group, then sleeps out the rest of 4 ms. At a million users a loop would otherwise wake for nearly every
request and every reply. A pass that fills its event array (512) goes straight on, so the cadence does not cap
throughput. The cost is a few milliseconds of latency per request.

**Clients.** Requests are parsed in place from a 64 KB receive buffer. A response is one `writev` (head and body),
so everything but a seed-sized feed leaves as one TCP segment. A connection's state is a 16-byte slot indexed by
its descriptor; buffers exist only while a request or response is split across reads or writes. While a request is
at Postgres, or a response is waiting for the socket, later requests on that connection wait (up to 80 KB), so
responses stay in order and a client that does not read cannot make the server buffer its answers.

**Reads.** A pass's reads go to the read connection with the fewest replies outstanding, as Bind/Execute pairs
under one Sync, 64 at most; more go to the next connection. A read connection's buffer (256 KB) holds the whole
reply to a full batch. No Describe is sent, so a reply is BindComplete, DataRows and the end of the statement
and nothing else. A read's Execute asks for as many rows as it can have (20 for a feed, one for a post), so that
end is the 5-byte PortalSuspended instead of a CommandComplete of 14 or 15 bytes; the SQL is the SPEC's either
way. Each read is answered as soon as its own end arrives. The JSON is written straight from the DataRow bytes.

**Writes.** Writes use connections of their own, so a commit never holds up a read. A pass's writes form one
group under one Sync (128 at most): one implicit transaction, one commit. Every write of the group is answered
only when that Sync's ReadyForQuery arrives, which Postgres sends after the commit. A write connection carries one
group at a time; while its commit is out, new writes wait and leave together as the next group. If a write fails
(a like on a missing post), Postgres rolls the group back: the failing write gets its own answer (404), and the
others are sent again, each alone between two Syncs, so each commits or fails by itself.

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
| One epoll loop per core, `SO_REUSEPORT` per loop | no locks and no hand-off between threads on the request path |
| Passes of 4 ms | one wake-up serves everything that arrived meanwhile; a pass's reads share a Sync and its writes a commit. Against 2 ms: half as many batches, and 6% less CPU on four CPUs at 950,000 users on a rented copy of the benchmark's layout, the packet work included |
| A pass's reads on one connection under one Sync | one packet to Postgres and one wake-up of one backend per batch; replies come back in full packets |
| No Describe, so no RowDescription | a reply carries the rows and nothing about their columns, which libpq asks for with every query |
| `id` and `like_count` as text, `created_at` binary | per column, the shorter format: a few digits against 8 and 4 bytes, 8 bytes against 29. Postgres replies are most of the bytes coming into the box |
| Separate write connections, group commit | a commit only delays other writes; one WAL flush for a pass's writes |
| `TCP_QUICKACK` off on the listener after `listen()`, which accepted sockets inherit; set again after a response slower than 30 ms | the kernel would ACK a request at once in a packet of its own; this way the response carries the ACK. Set on the listener it already holds for a connection's first request, which arrives before the server sees the connection: one packet fewer per new connection, and no system call per accept |
| `TCP_DEFER_ACCEPT`, read straight after `accept` | a new connection arrives with its first request, so one wake-up serves both |
| `TCP_THIN_LINEAR_TIMEOUTS` on client and Postgres sockets | with few packets in flight a segment lost twice waits two retransmit timeouts, not three |
| Loop threads in `SCHED_IDLE` | on a shared core the kernel's receive work (`ksoftirqd`) runs first, so the card's single queue does not overflow behind a busy loop |
| Lone Sync on a silent Postgres connection | earlier loss detection than the retransmit timer |
| gzip for bodies of 1 KB or more (in practice the feed) | a feed of the load test's posts is about 3.7 KB, three segments; compressed it is about 1.1 KB, one segment |
| Own gzip encoder: one fixed-Huffman block, greedy matches from a 4-byte hash table (16 KB) that is never cleared, a search at every other byte in runs without a match, the distance code computed instead of looked up, two ASCII literals written as two whole bytes, CRC-32 by carry-less multiplication | about 3 µs per feed here, measured over feeds that all differ (one feed repeated lets the processor learn its branches and reads 1.9 µs); at this size a general library's setup costs more than the work |
| 16-byte connection slot, no per-connection buffers | 240,000 open connections cost 11 MB of the server's own memory |
| Descriptors closed at the end of a pass | a number cannot be handed out again while the pass still holds events for its previous life |
| A connection's slot asked for ahead of its use: three events ahead while a pass's events are worked off, and two replies ahead while a batch from Postgres is read. Likewise the record of the operation a reply belongs to, and the next free records of the pools | slots are indexed by descriptor, so two events in a row are rarely near each other and the first look at a slot waits for memory |
| A feed's timestamp text kept from row to row up to the second, digits two at a time, written as one store of 16 bytes and one of 8 | under load the 20 rows of a feed are of the same second or two, which leaves the milliseconds to work out: 70 ns per feed against 445 (100 ns when every row is of another second). Kept for one response only |
| HMAC key blocks absorbed once; SHA-256 with its message schedule worked out four words per step (SSSE3) beside the rounds; the signature and the payload decoded on the stack, and a payload of plain strings and whole numbers read without the general JSON reader | a token check is four blocks of SHA-256, and the benchmark box has no SHA instructions: 124 ns a block against 190, and 790 ns a check against 1,510. Any other payload goes to the JSON reader as before; nothing is remembered between requests |
| A row of a response is written into one reservation; its strings are copied 16 bytes per step (SSE2) and checked for characters to escape on the way, likewise the search for the end of a request head and for its line ends and colons | a feed's 40 strings are 1.9 KB, and nearly all of them need no escaping: 200 ns per feed against 1,250 ns byte by byte. Reading a feed request takes 49 ns against 80, one with a token 82 against 188 |

## Rules notes

- **Rule 3.** The five statements are exactly those of `SPEC.md` (the feed with the literal `LIMIT 20`), plus
  `SELECT 1` for `/health`: one per request, prepared and pipelined. The result formats are a protocol choice; the
  SQL is the same. A read is sent a second time only after Postgres skipped it (an earlier statement of its batch
  failed) or its connection was lost, never while its first copy may still run. There is no hedging.
- **Rule 5.** 80 connections to Postgres; whatever is configured, the code caps the total at 512.
- **Rule 7.** Nothing is cached across requests: every response is built from a query run while serving it. The
  gzip table holds positions in the current input only; entries from earlier calls read as empty.
- **Rule 8.** A write is answered only at the ReadyForQuery of the Sync that commits it. `synchronous_commit` is
  not touched.
- **Rule 9.** Every request's signature and `exp` are checked; a token without `exp` is refused. No verification
  is remembered.
- **Rule 12.** Built from source by `build.sh` with the toolchain `install.sh` pins. No prebuilt binaries.
- **Rule 13.** No CPU pinning and no option for it. No file outside the folder is read (`sched_getaffinity` counts
  the CPUs). `SCHED_IDLE` is the policy of this process's own threads; the TCP options are per socket.

## Measured locally

Docker on one desktop (Intel i5-14400), laid out like the benchmark: the server in a container on 4 CPUs,
Postgres 18.6 with the planner settings of `box/postgresql.conf` on 6 others, `bench/mload` with `-gzip` on the
remaining 6. Clients and Postgres are on separate networks and segmentation offload is off, so the server's
interface counters are TCP segments as a wire carries them. CPU is the container's cgroup time (user + system).
`c-epoll-unorig-username` (rank 1 when this was written) ran through the same rig, alternating with this entry.
This shows what a request costs here; it says nothing about the box's network ceiling.

The tables below were measured with the first version (`f5d3314`, scored on the rig at 1,000,000 / 1,480,000 /
1,420,000). The current version carries over the changes measured in the io_uring entry
(`rust-iouring-mehrojmajidov`, #125) that do not depend on io_uring: 4 ms passes, Postgres before the listeners,
reads that ask for exactly their rows, `TCP_QUICKACK` on the listener, the faster token check, row writing, head
search and gzip, and prefetched slots. Against the first version, with the server held to 30% of one CPU so that
it gives out before Postgres and the generators do (`-think-scale 0.1`, 45,000 to 55,000 users):

| 30% of one CPU | first version | this version |
|---|---:|---:|
| Most req/s carried | 40,000 | 48,000 |
| Server CPU per request, 45,000 users | 7.5 µs | 5.5 µs |
| Highest step that passed (p95 < 500 ms, p99 < 1 s) | 47,500 | 55,000 (the top of the range) |

| 40,000 users, `-think-scale 0.1`, about 42,000 req/s (two rounds each) | this entry | c-epoll |
|---|---:|---:|
| Packets into the server per request | 3.01 | 3.01 |
| Bytes into the server per request | 1,607 | 1,608 to 1,609 |
| Response bytes out per request | 655 | 700 |
| Server CPU per request | 7.8 and 8.0 µs | 9.5 and 8.4 µs |
| p95 / p99 over the run | 4.3 / 12 ms | 7 to 15 / 20 to 38 ms |

| 100,000 users, `-think-scale 2.5` (the scoring think time, with the clients' keepalive probes) | this entry | c-epoll |
|---|---:|---:|
| Packets into the server per request | 4.24 | 4.26 |
| Bytes into the server per request | 1,678 | 1,688 |
| Server CPU per request | 15.2 µs | 14.9 µs |

- **240,000 users** at `-think-scale 2.5` (four generators): p95 4.1 ms, p99 4.2 ms, no errors, no retransmits.
  The server's resident memory was 11 MB; the kernel's socket memory was about 3.8 KB per connection.
- **1% loss** on everything arriving at the server (`netem`), 20,000 users at `-think-scale 0.1`: p95 4 ms and
  p99 206 ms for this entry, 4 to 8 ms and 208 ms for c-epoll. The p99 is TCP's retransmit timer in both.
- **Every Postgres backend killed** in the middle of a run (3,000 users, 80 connections terminated at once): no
  failed request, the slowest took 86 ms, and all 80 connections were re-established.
- **`test/run.sh`:** `passed=42 failed=0` in a clean `ubuntu:24.04` container after `install.sh`.
- The entry's own tests (`cargo test`, 40 tests) cover the token rules and the token payload's short way, JSON
  reading and escaping, row writing, timestamps, HTTP framing, head search and routes, the Postgres messages,
  and gzip round trips through the `gzip` tool.

## Configuration

Only the variables of `SPEC.md` are needed. These are optional and were used for the measurements above:

| Variable | Default | |
|---|---|---|
| `FEED_LOOPS` | the CPUs in the affinity mask | loop threads |
| `FEED_PG_READERS`, `FEED_PG_WRITERS` | 48 and 32 divided by the loops, at least 2 | Postgres connections per loop |
| `FEED_PASS_US` | 4000 | length of a pass in microseconds (0: no pause) |
| `FEED_SCHED_IDLE` | 1 | 0 leaves the loop threads in the default scheduling class |
| `FEED_READS_IN_FLIGHT` | 0 (no limit) | an experiment, off: at most this many reads per loop at Postgres; the rest wait in the loop, and once the oldest has waited `FEED_BACKLOG_AGE_US` (10000) the newest go first |

## The code

- `src/main.rs`: configuration and startup. `src/sys.rs`: the libc calls and constants.
- `src/reactor.rs`: the loop and its clients (accept, read, respond). `src/db.rs`: the Postgres side of a loop
  (batches, write groups, the reply stream, lost connections).
- `src/pg.rs`: login, prepared statements and request messages. `src/http.rs`: request parsing, routes, heads.
- `src/json.rs`, `src/jwt.rs`, `src/crypto.rs` (SHA-256, HMAC, PBKDF2, base64), `src/gzip.rs`, `src/time.rs`.

## Running

```bash
sudo ./install.sh   # gcc as the linker, Rust 1.99.1 into /opt/feed-rust
./build.sh          # target/release/feed
DATABASE_URL=postgres://user:pass@host:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 ./start.sh
```
