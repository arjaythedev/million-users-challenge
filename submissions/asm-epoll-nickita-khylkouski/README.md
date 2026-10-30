# asm-epoll — x86-64 assembly, no libc

A static binary written in GNU assembler (Intel syntax). No libc, no runtime, no driver library: raw
syscalls, one epoll loop per core, a Postgres wire-protocol v3 client (SCRAM-SHA-256, extended query,
pipelined), HTTP/1.1, JSON, HMAC-SHA256 and gzip are all in `src/`. Build dependency: `binutils` 2.42
(Ubuntu 24.04's package, which `install.sh` installs). Runtime dependencies: none.

```
bash install.sh      # apt-get install binutils
bash build.sh        # as + ld  ->  build/server
bash start.sh        # reads DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT
```

Behaviour (status codes, error texts, validation order, JSON byte layout, SQL text) follows the challenge
spec and the reference `c-epoll` entry exactly, so the golden files match byte for byte. `test/run.sh`
passes 42/42.

## Architecture

Everything is built around one question: how many idle-but-live keep-alive connections can one 16-vCPU
box with a single-queue NIC keep answering within the latency budget. At a million connections the work per
request is small; what runs out first is expected to be packets per second on the one core that takes the
NIC interrupt, and memory, so the design spends effort on both and little on HTTP or JSON speed.

**Workers.** One thread per CPU, pinned, with its own `epoll` set, its own `SO_REUSEPORT` listening sockets
on both ports, and its own Postgres connections. Nothing is shared between workers after start: no locks,
no atomics, no cross-thread queues. The only process-wide table is the connection slot array (32 bytes per
fd, indexed by fd), which a worker only touches for its own fds.

**The interrupt core is skipped.** With a single-queue NIC every packet first lands on one CPU (the
interrupt, NAPI polling, GRO and the RPS hand-off to the other CPUs), on top of its own share of the TCP work.
That CPU gets no worker. At start the server looks in `/proc/interrupts` for virtio-net's receive-queue line
(`virtioN-input.Q`). With exactly one, the CPU named in `/proc/irq/N/effective_affinity_list` is skipped, or,
if that file lists several CPUs, the one whose column counts the most interrupts. With several receive queues
nothing is skipped. Without such a line (another NIC driver), `/proc/softirqs` decides: a CPU whose `NET_RX`
count is more than four times every other CPU's is skipped. This only applies with 4 or more CPUs, and
`SKIP_IRQ_CPU=0` turns it off.

**Flow steering.** Each port's listener group gets a classic BPF program (`SO_ATTACH_REUSEPORT_CBPF`) that
returns the index of the worker pinned to the CPU on which the SYN was processed (`SKF_AD_CPU`). For a CPU
without a worker it returns an out-of-range index, and the kernel falls back to its usual hash. With RPS
spreading flows over all CPUs by hash, a connection's softirq work and the worker that reads and writes its
socket are then on the same core: the socket's data stays in one cache and no cross-CPU wakeup is needed.
(The box also has RFS on, which moves a flow to its reader's CPU once the socket has been read; steering
puts it there from the SYN on, including the accept wakeup.)
The spread of new connections is now only as even as RPS makes it, so worker 0 checks it once a second.
If one worker took more than twice its even share of a window of at least 64 new connections per worker,
the program is detached from both groups and the kernel's hash takes over (`STEER=0` turns steering off).

**One packet in, one packet out.** Every response is one contiguous buffer (head + body) handed to a
single `sendto`, so it is one skb (one or two frames) and carries the ACK of the request with it. For
that the socket has to be in the kernel's delayed-ACK ("pingpong") mode; otherwise a request that comes
after a pause longer than the RTO, which is every request at these think times, is ACKed at once in a
packet of its own. Three things keep the socket in that mode (all checked with tcpdump on Linux 6.8 and 7.0):

- `TCP_QUICKACK` is cleared on the listeners *after* `listen()`. `inet_csk_listen_start()` resets the
  listener's ACK state, so set before `listen()` the option never reaches the accepted sockets: the first
  request of every connection then draws a pure ACK. Set after, its ACK rides on the response.
- Responses go out within a few milliseconds. The delayed-ACK timer is 40 ms at any HZ
  (`TCP_DELACK_MIN`), and when it fires the socket leaves pingpong mode.
- When a response was late anyway (`QUICKACK_MS`, default 25 ms, counted from when the request reached the
  kernel, which `TCP_INFO`'s `tcpi_last_data_recv` tells for the oldest request of each epoll batch), the
  server sets `TCP_QUICKACK=0` on that socket again. Without that the next request would draw a pure ACK
  of its own before the socket re-enters the mode. Responses that are late every time gain nothing: the
  timer's ACK goes out either way.

(A request that arrives in two segments, or is larger than one MSS, is ACKed at once whatever the mode;
the benchmark's requests are single writes under 1 KB, so that does not come up.)
`TCP_DEFER_ACCEPT` means accept only returns when the first request has arrived, so there is no idle
wakeup per connection (it changes no packets). Bodies of 1 KB or more are gzipped when the client accepts
gzip. On the seed data the feed is 5.1 KB and gzips to 1.9 KB: two segments in one skb instead of four, which
the client's GRO takes as one packet and answers with one ACK. Under the load generator, whose posts are
short, the feed is about 3.6 KB and gzips to about 760 bytes, one segment. What is left per request on a
warm connection is the request, the response and the client's one ACK of it, plus what the client adds on
its own (the Go dialer's 15 s keepalive probes and their answers, about one pair per request at the
scoring think times).

**Postgres.** Every worker owns a few pipelined connections (`PG_CONNS` in total, default 64, divided over
the workers). Statements are appended to a connection's output buffer as `Bind`/`Execute` and sent once
per event-loop iteration behind a single `Sync`, so one round trip and one pair of packets carry a whole
batch of feed and post reads. Writes (`POST /posts`, likes) get their own `Sync` and are answered only
when the `ReadyForQuery` after it arrives, which is when the transaction is committed. Results are read
back in order through a small op ring per connection; a late result for a connection that has since been
closed and reused is detected with a generation counter and dropped. A connection that dies answers its
pending ops with 500 and is reconnected once per second. Health is a real `SELECT 1` through the same
path; without a working connection it is answered 503 immediately.

**Memory per connection.** Idle connections own nothing but their 32-byte slot. A read buffer is borrowed
from the worker while a request is parsed and only kept (allocated from a per-worker size-class
allocator) when a request arrives in pieces or when output has to wait for `EPOLLOUT`. A million
connections therefore cost the process about 32 MB plus the kernel's socket buffers.

**Batching.** When an iteration woke up for only a handful of events (`BATCH_MIN`, default 32) the loop
sleeps `BATCH_US` (default 2000 µs) before parsing them, so that more requests share one Postgres round
trip and one `sendto`. Both are environment knobs for A/B runs; `BATCH_US=0` disables it.

**Robustness under overload.** `accept` backs off for a second on `EMFILE`/`ENFILE` instead of spinning;
requests larger than 64 KB and chunked bodies drop the connection; the op ring and output buffers have
hard caps; nothing allocates per request on the fast path; no request ever gets a 5xx because of load
alone (only a failed database round trip).

## Environment

| variable | default | meaning |
|---|---|---|
| `DATABASE_URL`, `JWT_SECRET`, `HOST`, `PORT`, `EXTRA_PORT` | challenge values | as in the spec |
| `WORKERS` | CPUs (minus the interrupt core) | fewer workers |
| `PG_CONNS` | 64 | Postgres connections, total (at least 2 per worker) |
| `SKIP_IRQ_CPU` | 1 | leave the NET_RX core without a worker |
| `STEER` | 1 | CBPF reuseport steering by CPU |
| `DEFER_ACCEPT` | 1 | `TCP_DEFER_ACCEPT` seconds on the listeners (0 = off) |
| `QUICKACK` | 0 | 1 keeps the kernel default quick-ACK behaviour |
| `QUICKACK_MS` | 25 | re-arm delayed ACKs on a socket after a response this late (0 = off) |
| `BATCH_US` / `BATCH_MIN` | 2000 / 32 | event-loop batching, see above |

The server logs its choices at start: `workers`, `net_rx cpu` (the skipped CPU, -1 for none), `nic rx
interrupt` (when `/proc/interrupts` was read), `postgres connections per worker` and `cpu steering program`
(0 = attached). If the guard detaches the program later it logs `cpu steering detached` with the busiest
worker's share.

## Testing

`bash test/run.sh submissions/asm-epoll-nickita-khylkouski` passes all 42 checks on Ubuntu 24.04 and 22.04
(whose binutils 2.38 builds a byte-identical binary). Beyond that:

- **Protocol probe.** Pipelined requests, gzip round trips, `Expect: 100-continue`, requests split across
  packets, `Connection: close`, HTTP/1.0, oversize requests, hundreds of concurrent pipelined connections,
  JWT edge cases (no `exp`, an `alg` other than HS256, a bad `sub`), and Postgres killed mid-run.
- **Load.** The challenge's `bench/mload` with 20,000 users on 4-vCPU GitHub Actions runners: no errors,
  p95 5.8 ms, p99 6.4 ms, server RSS 7.4 MB.
- **Steering**, on real x86-64 kernels (GitHub Actions runners with Linux 6.8 and 7.0); x86 emulators refuse
  `SO_ATTACH_REUSEPORT_CBPF`. The program attaches, and every connection from a client pinned to one CPU
  lands on that CPU's worker, a different worker for each CPU. When a single CPU opens all the connections
  the guard detaches the program, and the next connections spread evenly.
- **Interrupt-core detection**, against fake `/proc/interrupts`, `/proc/softirqs` and affinity files: one,
  several or no receive-queue lines; single-CPU, range and list affinities; missing and empty files; zero
  counts; a file larger than the read buffer. Each case was started 100 times on both kernels.
- **Delayed ACKs**, with tcpdump on loopback on both kernels, with the same counts: one keep-alive
  connection, requests 1.5 s apart, and two responses made 300 ms late, first by stopping Postgres and then
  by stopping the server.

| settings | pure ACKs from the server |
|---|---|
| defaults | 2: the delayed-ACK timer's, on the two late responses |
| `QUICKACK_MS=0` | 4: plus one on each request after a late response |
| `QUICKACK=1` | 3: plus one on the connection's first request |

## Credits

- `rt.s`, `cpu.s`, `crypto.s`, `pg.s`, `http.s`, `json.s` and `gzip.s` come from the author's
  `asm-epoll-nickita-khylkouski` entry in the multicore-server-challenge (MIT). `main.s`, `server.s`,
  `pgio.s`, `api.s`, `render.s` and `sql.s` are new for this challenge.
- The exact response and validation semantics follow unorig-username's `c-epoll` reference entry, as the
  golden files do.
- `gzip.s`: deflate design and parameters follow libdeflate (Copyright 2016 Eric Biggers, 2024 Google LLC;
  MIT). The PCLMULQDQ CRC32 is translated from Chromium zlib's `crc32_sse42_simd_` (Copyright 2017 The
  Chromium Authors; BSD-3-Clause). Full notices are in `src/gzip.s` and `LICENSE`.

## Layout

```
src/
  main.s    entry, env, IRQ-core detection, listeners + CBPF steering, worker spawn
  server.s  epoll loop, accept, client read/parse/send, parked output, close
  api.s     routing, JWT verification, body validation
  pgio.s    pipelined Postgres I/O, op ring, Sync discipline, reconnects
  render.s  JSON bodies, static error responses, gzip + head in one send
  sql.s     the five prepared statements
  rt.s      allocator, threads, memcpy, time        http.s    HTTP/1.1 parser and head writer
  cpu.s     CPU feature detection                   json.s    JSON reader and writer
  crypto.s  SHA-256, HMAC, base64url                pg.s      Postgres protocol v3 (SCRAM, binary params)
  gzip.s    deflate + CRC32                         defs.inc / app.inc  constants and struct layouts
```
