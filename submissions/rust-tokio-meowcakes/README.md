# rust-tokio-meowcakes

| | |
|---|---|
| Language | Rust 1.94.0 (edition 2024), rustup in `/opt/twelve-rust` like the repo's Rust example |
| Runtime | tokio 1.54.1: a current-thread runtime + `LocalSet` per core |
| Client sockets | io_uring via io-uring 0.7.15 (direct descriptors, multishot accept and receive, provided buffer ring); tokio `TcpStream`s with `IO_URING=0` |
| HTTP | hand-written HTTP/1.1 on httparse 1.10.1 |
| Postgres | own wire-protocol client (`src/pg.rs`); postgres-protocol 0.6.12 only for the SCRAM-SHA-256 login |
| gzip | own encoder (`src/gzip.rs`), crc32fast 1.5.4 for the CRC-32 |
| Other crates | libc 0.2.186, hmac 0.13.0 + sha2 0.11.1 (HS256), base64 0.22.1, serde_json 1.0.154 (request JSON), itoa 1.0.18, socket2 0.6.5; libdeflater 1.26.1 in tests only, to check the gzip output |

Every version is pinned in `Cargo.toml` (`=x.y.z`) and `Cargo.lock` (`cargo build --locked`).

## Running

```bash
sudo bash install.sh   # build-essential, curl, Rust 1.94.0 (idempotent)
bash build.sh          # target/release/feed (-C target-cpu=native, fat LTO)
DATABASE_URL=postgres://feed:pw@db:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

Optional knobs for experiments (none is set on the benchmark box): `IO_URING` (default 1; 0 = tokio's epoll),
`BATCH_WAIT_US` and `BATCH_MIN` (io_uring batching, default 5000 and 16; `BATCH_WAIT_US=0` turns it off),
`THREADS`, `PG_READ_CONNS` and `PG_WRITE_CONNS` (per thread), `GZIP_MIN` (bytes, default 1024).

## How it works

- **One thread per core** (`available_parallelism`, so it follows the CPU affinity mask and cgroup quota on 8 and
  4 cores). Each thread runs its own current-thread tokio runtime and binds its own `SO_REUSEPORT` listener on both
  ports, so the kernel spreads connections over the threads. Each thread owns its own Postgres connections. Threads
  share only the read-only config: there are no locks and no cross-thread wakeups.
- **Client sockets on io_uring** (`src/uring.rs`). Each thread has a ring with a multishot accept on each
  listener and a multishot receive per connection. Received bytes land in a ring of 8192 × 2 KB buffers the kernel
  picks from, and each buffer goes back as soon as its bytes are handled. Responses go out as send submissions, and
  one `io_uring_enter` per pass submits all of them, instead of a `recv` and a `send` syscall per request plus epoll
  per socket. A connection is a ~100-byte slot in a slab, not a task. Its database answer is delivered by a
  callback that queues the send, and a client's half-close still gets every answer it is owed. If the ring can't be
  set up, the thread falls back to the epoll path.
- **Direct descriptors.** Connections are accepted straight into the ring's file table, so the process holds no fd
  per connection: the ring doesn't look up an fd on every operation, and the fd table doesn't grow (with RCU waits)
  during the ramp. Socket options on them are set through the ring (`SOCKET_URING_OP_SETSOCKOPT`, kernel 6.7+). A
  connection is closed by cancelling its armed receive, which holds the socket open, hard-linked to closing the
  descriptor.
- **The buffer ring on Ubuntu's 6.8 kernels.** Ubuntu's 6.8.0-1xx kernels (-132, -135..-142, -145..-149; the box
  has run -147) refuse to register a provided-buffer ring with zeroed reserved fields: the check is inverted. The
  ring is registered by hand, the upstream way first and then with `resv[0] = 1`, which those kernels take. Without
  that the thread would fall back to the epoll path.
- **Batching.** Completions are deferred (`DEFER_TASKRUN`), so the kernel doesn't wake the thread for each one.
  After a pass that did work, the thread waits in `io_uring_enter` until 16 completions are in or 5 ms has
  passed. Every wakeup, submit and Postgres round trip then covers many requests, at a cost of a few ms of
  latency. An idle thread waits for the first completion. The thread only ever sleeps in the ring. Postgres stays
  on tokio `TcpStream`s, which tokio polls without blocking between waits, and a multishot poll on each Postgres
  socket wakes the ring wait when a reply arrives. (Sleeping in tokio's epoll with an eventfd registered on the
  ring was tried: the kernel signals that eventfd for deferred completions only after an RCU grace period, 5 to
  20 ms late.) Measured with 20k connections at ~39k req/s on 4 cores: 20.8 µs of CPU per request without
  batching, 11.3 µs with it; about the same from 8 to 128
  completions.
- **The epoll path** (`IO_URING=0`, `src/http.rs`): a tokio task per connection. An idle connection owns no buffer:
  it reads into the thread's 64 KB buffer, parses and answers there, and writes the response with one `send` from
  the thread's output buffer. Only a partial request, requests pipelined behind a database wait, or a response the
  socket couldn't take at once is copied into a per-connection `Vec`. It reads with tokio's `poll_read`, which treats
  a short read as "drained" and skips the extra `recv` that would return `EAGAIN`.
- **Fewer ACK packets.** The listeners are in delayed-ACK mode (`TCP_QUICKACK` off), which accepted sockets
  inherit. Otherwise a new connection, and any request after an idle gap longer than the retransmission timeout (a
  client sends one about every 24 s), gets a separate ACK before its response. Since the mode doesn't stick once
  the kernel's delayed-ACK timer fires, a connection whose response took 25 ms or more gets it again. Measured with
  100k users at the benchmark's think times, ramp included: 6.35 packets per request instead of 6.81.
- **Postgres reads are pipelined under one Sync.** A request queues its query on its thread and waits. The first
  query queued wakes a flusher task, which tokio runs after every task already ready. So everything queued in that
  pass of the event loop goes to the least loaded read connection as Bind/Execute pairs under one Sync, in one
  write, with up to 64 reads per Sync (more are split over connections). Each read is answered as soon as its
  `CommandComplete` arrives. If one fails, the reads after it, which Postgres skipped, are sent again.
- **Writes use group commit.** New posts and likes queue for the thread's write connections. An idle write
  connection takes everything queued (up to 256) as one implicit transaction: Bind/Execute pairs under one Sync,
  one commit. While it commits, the next group builds up. Every write in the group is answered only after its
  `ReadyForQuery`, so it is committed. If one fails, the whole transaction rolled back: the failed write gets its
  answer (SQLSTATE 23503 on a like → 404 `post not found`), and the others are sent again.
- **Only the SPEC's statements**, prepared once per connection, with binary parameters and results. Ids, counts
  and timestamps need no text parsing, and `created_at` is formatted straight from microseconds (UTC, truncated to
  the millisecond).
- **Connections**: by default 96 read and 32 write connections in all, and at least 2 + 1 per thread (4 cores: 24 + 8
  per thread; 16 cores: 6 + 2; 128 in all either way, of the 512 allowed). Postgres runs each connection's pipeline
  on one backend, so near the limit more connections mean shorter queues behind each backend. Each gets a large receive buffer before it connects (the box's `tcp_rmem` starts
  sockets at 16 KB) and replies are read 64 KB or more at a time. A lost connection hands its unanswered reads to another connection and fails its
  writes with 500 (their commit is unknown), then reconnects every second. `/health` answers 503 with the last
  error while a thread has no live connection.
- **JWT**: the HMAC-SHA256 signature is checked first, with the key schedule computed once and cloned per request.
  Then the header: the usual `{"alg":"HS256","typ":"JWT"}` bytes are accepted as-is; anything else is decoded and
  must say HS256. Then `exp` (required) and `nbf`, then `sub` (a positive integer string) and `username` (a
  string). Nothing is remembered between requests.
- **Gzip, own encoder** (`src/gzip.rs`), for bodies of 1 KB or more when the request accepts gzip. In practice only
  the feed qualifies. It writes one deflate block with the fixed Huffman codes, so there are no trees to build
  or send. Matches are greedy LZ77 from a 4-byte hash table that is never cleared: positions are stored offset
  by a base that moves past each call's input, so earlier calls' entries simply fail the range check. On the feed
  as it looks under load (3.6 KB of load-test posts) it takes 1.5 µs, against 5.5 µs for libdeflate level 1.
  The output is 893 bytes instead of 719, still one TCP segment with the headers. Measured on 4 cores at about
  170k requests/s, server CPU per request went from 18.1 µs to 16.0 µs. Its reads are unchecked and its bit writer
  is branch-free (it always stores 8 bytes and advances by the whole bytes filled), which took another 8% off
  (1.52 → 1.40 µs). Hinting the repeated JSON keys to it as known matches made it slower: the hash finds each key
  with a single lookup anyway. The tests round-trip its output through libdeflate, from empty input to 300 KB.
- **JSON escaping** looks for `"`, `\` and control characters 8 bytes at a time and copies the clean runs whole.
  Together with the faster gzip, that took server CPU per request from 11.1–11.2 µs to 10.7–10.9 µs (20k
  connections at ~39k req/s on 4 cores). A test checks it against a byte-at-a-time escaper on random input.

## The code

- `src/main.rs`: config, one thread and runtime per core, the per-thread `Worker`
- `src/uring.rs`: the io_uring socket layer
- `src/http.rs`: request parsing, routing and responses (shared), and the epoll socket layer
- `src/db.rs`: per-thread query queues, the flusher, read batches and write groups, reply decoding
- `src/pg.rs`: Postgres login, prepared statements, message encoding
- `src/json.rs`: response JSON, timestamps, the `POST /posts` body
- `src/jwt.rs`: token checks
