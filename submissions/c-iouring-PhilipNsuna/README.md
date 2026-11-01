# c-iouring-PhilipNsuna: C, one io_uring (or epoll) loop per core, own HTTP/1.1, own Postgres wire client

| | |
|---|---|
| Language | C (gcc 13 from Ubuntu 24.04, `-O2 -march=native -flto`), no framework, no libraries beyond libc, pthread and liburing |
| IO | one loop per CPU in the affinity mask; **io_uring** (liburing 2.5 from the archive: multishot accept, multishot recv with kernel-provided buffers, SEND, one `io_uring_enter` per iteration) with automatic fallback to **epoll** |
| Postgres | own v3 wire-protocol client (`src/pg.c`): SCRAM-SHA-256 with its own SHA-256/HMAC/PBKDF2, prepared statements, pipelined, binary results |
| HTTP / JSON / JWT | hand-written (`src/http.c`, `src/json.c`, `src/jwt.c`) |
| Compression | own gzip encoder with a fixed Huffman code (`src/deflate.c`), bodies >= 1 KB |
| Front | direct, `PORT` and `EXTRA_PORT`, `SO_REUSEPORT` listeners per loop, accepts steered to the CPU that handles the flow (`SO_ATTACH_REUSEPORT_CBPF`) |
| Postgres connections | 128 (96 read + 32 write, split over the loops) |

The design came out of a Go prototype of the same shape (raw epoll, own wire client, own gzip)
whose profile showed two thirds of the CPU in the kernel's per-syscall TCP work. This C port keeps
the design and adds what Go could not do: io_uring submission batching, CPU-local accept steering
and thread pinning. Everything below was then measured on two real Linux boxes with a NIC between
them, because the loopback rig had hidden the differences that matter (see "Where it stands").

## Running

```bash
sudo bash install.sh    # build-essential, liburing-dev
bash build.sh           # bin/feed
DATABASE_URL=postgres://feed:pw@db:5432/feed JWT_SECRET=million-users-challenge HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

Knobs (env, optional): `LOOPS` (default: CPUs in the affinity mask), `DB_READ` / `DB_WRITE` (96 / 32),
`BATCH_MIN` / `BATCH_MICROS` (32 / 8000), `GZIP_MIN` (1024), `URING` (1: io_uring, falls back to epoll
if the kernel refuses; 0: epoll), `PIN` (1: pin loop i to the i-th CPU of the mask), `CBPF` (1: steer
accepts by CPU; needs `PIN` and a contiguous CPU set, otherwise off), `STATS_SECS` (print per-loop
stats every N seconds). `EXTRA_PORT` also takes a comma list (`3001,3002,3003`): a convenience for a
load generator with one IP, which runs out of source ports at ~64k connections per server port.

## Design

- **Loops.** One thread per CPU of the affinity mask, pinned to it (`PIN=1`; a setting inside the
  process, like thread counts: nothing on the box is touched). Each has its own listeners on both
  ports, its own Postgres connections and its own ring or epoll set. Nothing is shared.
- **Accept steering.** All listeners of a port form one `SO_REUSEPORT` group; a classic BPF program
  attached to the group returns `cpu - base`, so a new connection lands on the loop that runs on the
  CPU where the kernel is already processing its packets (RPS on the box's single-queue NIC). No
  cross-CPU wake-ups or cache transfers for the request path.
- **io_uring** (`src/uring.c`): multishot accept per listener; a multishot recv per connection that
  takes a buffer from a pool of 1,024 x 8 KB per loop provided with the legacy
  `IORING_OP_PROVIDE_BUFFERS` (Linux 5.7; the ring-mapped buffer ring is what returned `EINVAL` on
  the box's 6.8 kernel for two other entries); each response is one `SEND` from a pooled buffer; the
  Postgres sockets are watched with multishot `POLL`. Every iteration is one `io_uring_enter` that
  submits everything queued and waits for `BATCH_MIN` completions or `BATCH_MICROS`: the batching
  window without a sleep. An fd is closed only once the ring holds nothing on it (shutdown, then the
  last completion closes), so a late completion never hits a reused fd. If the ring cannot be set up
  or an opcode is missing, the loop uses epoll.
- **epoll** (`src/loop.c`): level-triggered, one read per event into a 64 KB scratch, requests
  parsed in place, one write per response, a `BATCH_MICROS` `nanosleep` after a light iteration so
  that work still arrives in batches.
- **An idle connection is 48 bytes** in a table indexed by fd (`conn_t`), plus its kernel socket.
- **Postgres on the loop** (`src/pg.c`): Bind + Execute per request appended to a connection's buffer,
  one write per iteration, one Sync for the iteration's reads (one transaction, one round trip), a
  Sync per INSERT (its own transaction, committed before the response: rule 8). Results are decoded
  from the DataRow bytes straight into the response. A lost connection fails what it had in flight
  (500, `/health` 503) and is reopened from the loop with a 1 s retry.
- **gzip** only for bodies >= 1 KB when the client accepts it: `src/deflate.c`, a fixed-code
  encoder whose literal/length table is trained at startup on `feed_sample.json` (a sample feed
  page shipped with the source, used for nothing else), with a slicing-by-8 CRC32.

## Rules notes

- The SQL is `SPEC.md`'s, verbatim, prepared once per connection, one statement per request; nothing
  moved into Postgres. No caching across requests; every token's signature, `alg` and `exp` checked.
- Writes commit before their response (autocommit, the response waits for CommandComplete).
- 128 Postgres connections (at most 512 allowed). Nothing on the box is tuned; the process raises its
  own soft `RLIMIT_NOFILE` and pins its own threads.
- Built from source on the box (`install.sh`: build-essential and liburing-dev from the Ubuntu 24.04
  archive; `build.sh`: one gcc invocation). No prebuilt binaries.

## Files

```text
install.sh / build.sh / start.sh    src/main.c    config, listeners, CBPF, threads, stats
src/loop.c  epoll loop, connections, HTTP handling, dispatch     src/uring.c   io_uring backend
src/pg.c    Postgres wire client (SCRAM, pipelining)             src/http.c    request parser
src/json.c  strict JSON scanner, escape, JS trim                 src/jwt.c     HS256 verification
src/resp.c  responses, post object, timestamps                   src/deflate.c gzip encoder, CRC32
src/sha256.c SHA-256, HMAC, PBKDF2                               feed_sample.json  gzip training sample
```

## How it was measured

Two rigs. The **local rig**: Docker on an Apple-silicon Mac, 11 vCPUs shared by the server (pinned
to 4), Postgres 18 and the load generator, all over loopback. The **real boxes**: two 4-vCPU Ubuntu
24.04 VPSs (Xeon Platinum 8260, kernel 6.8, one virtio NIC queue, 0.18 ms RTT), the server alone on
one (`box/setup-app-box.sh` applied, firewall to the other box only), Postgres 18 and the load
generator on the other. Every run resets the database to the seed and starts the server fresh.

The load generator is the repo's `bench/mload` with the scoring profile (`-gzip -think-scale 2.5`,
the 1M-users think time). Two regimes: **dense** = 15,000 users at think 0.1 (13.2k requests/s on
4 loops, which is the per-loop rate a million users put on 16 loops); **sparse** = 100,000 users at
the scoring profile (3k req/s, the realistic ratio of connections to requests). The load box's one
IP runs out of source ports at ~64k connections per server port, so sparse runs use four server
ports (`EXTRA_PORT=3001,3002,3003`); bigger user counts need more load machines, as the scoring
setup has.

Two figures per run: **process** CPU per request (user + sys of the server process, from
`/proc/<pid>/stat`) and **whole box** CPU per request (every core's user + nice + sys + irq + softirq
from `/proc/stat`, i.e. including the NIC's softirq work that happens outside the process;
iowait excluded since io_uring waits are accounted as iowait). Peak RSS from `VmHWM`; p95 / p99
from the load generator; TCP counters from `nstat` before and after.

The boxes drift about 10% over hours (shared hosts), so every verdict below comes from interleaved
A/B/A/B series run within the same hour, not from runs on different days.

## Journal

### Port, epoll backend first (local rig)

42/42 on the first run; the protocol edge cases (pipelining, split headers, bad lengths, early
close) pass. "T30" = 30,000 users at think 0.1 (31.5k req/s), "150k" = 150,000 users at the scoring
profile.

| T30 (31.5k req/s, 4 cores) | us/request steady | p95 / p99 | RSS |
|---|---:|---|---:|
| Go prototype (own wire client, own gzip, raw sleep) | 14-16 | 6 / 34 ms | 69 MB |
| C, epoll backend | 14.8-15.0 | 5.5 / 15.5 ms | 44 MB |

Same CPU per request as Go in the dense regime: the user-space work was already small; what is
left is the kernel. Memory is lower (no runtime, 48-byte connections).

### io_uring backend (local rig)

First version: 42/42, edge cases pass. A packing bug found with a completion trace (`URING_DEBUG=1`):
the generation field in `user_data` overlapped the type bits, so every recv looked stale and was
dropped; fixed by masking to 28 bits.

| | us/request steady | p95 / p99 | wakeups/s | RSS |
|---|---:|---|---:|---:|
| epoll, T30 | 14.8-15.0 | 5.5 / 15.5 ms | 1,680 | 44 MB |
| io_uring v1, T30 | 15.8-17.1 | 6.8 / 64 ms | 3,200 | 50 MB |
| epoll, 150k | 35-44 | 6.8 / 184 ms | 1,620 | 87 MB |
| io_uring v1, 150k | 39-61 | 26 / 220 ms | 1,700 | 99-123 MB |

v1 waited for `BATCH_MIN` completions *or* the window, so under load it woke as soon as 32
completions piled up and lost the batching (twice the wake-ups); every buffer re-provide also posted
a completion (3 per request), and a pipelined request copied through a `malloc`. v2: after a light
iteration the enter waits the whole window (the epoll backend's sleep, but as part of the same
syscall that submits the responses), `IOSQE_CQE_SKIP_SUCCESS` on re-provides, no copy.

| | us/request steady | p95 / p99 | wakeups/s | RSS |
|---|---:|---|---:|---:|
| io_uring v2, T30 | **14.3-15.6** | 6.6 / 76 ms | 2,470 | 50 MB |
| io_uring v2, 150k | **35-41** | 7.0 / 60 ms | 1,670 | 88-90 MB |

v2 equals the epoll backend in CPU per request on this rig, with the memory growth gone. On
loopback, the syscall entry/exit that io_uring saves is a small part of a `write` that also runs
the receiver's TCP path; a real NIC is where the difference shows (it did: below).

### Postgres batches as SEND requests (`PG_SQE=1`, io_uring only; local rig)

The Postgres write per batch per connection was the remaining syscall in the loop's own code path;
this variant hands the batch to the ring (double-buffered per connection, a SEND completion
releases the buffer and sends whatever queued meanwhile).

| | us/request steady | p95 / p99 |
|---|---:|---|
| io_uring v2, T30 | 14.3-15.6 | 6.6 / 76 ms |
| + `PG_SQE=1`, T30 | 14.4-15.4 | 6.3 / 67 ms |
| io_uring v2, 150k | 35-41 | 7 / 60 ms |
| + `PG_SQE=1`, 150k | 51-61 | 76 / 720 ms |

Dropped (off by default). Neutral when the loop is busy; in the sparse regime a batch that is
flushed while the previous SEND is still in flight waits for that completion, i.e. for the next
window, and the whole tail moves out by one window. The write syscall is cheaper than the delay.

### Database outage (io_uring backend)

All backends of a 2-loop server terminated under traffic: `/health` answers 503 degraded for
~200 ms, then 200; the requests in flight get 500, the rest 200; every connection is reopened by
the loop within its 1 s retry.

### Real boxes: io_uring vs epoll, interleaved

Four dense rounds and two sparse rounds, each round io_uring then epoll back to back, 2 ms window:

| | process us/req | whole box us/req |
|---|---:|---:|
| dense, io_uring (4-round mean) | 20.4 | 40.4 |
| dense, epoll (4-round mean) | 24.1 | 43.3 |
| sparse, io_uring (2-round mean) | 38.5 | 76.4 |
| sparse, epoll (2-round mean) | 39.2 | 79.3 |

io_uring wins every round: 15% in the process, 7% on the whole box. The whole-box gap is smaller
because the NIC's softirq work is the same for both backends; what io_uring removes is the per-call
entry/exit and the per-send TCP work being done one socket at a time.

### Real boxes: the batching window

Single runs per point, same hour, io_uring:

| `BATCH_MICROS` | dense: whole box us/req | dense p95 / p99 | sparse: whole box | sparse p95 / p99 |
|---:|---:|---|---:|---|
| 1000 | 45.1 | 4.8 / 16.6 ms | | |
| 2000 | 40.4 | 6 / 16 ms | 76.4 | 6 / 8 ms |
| 3000 | 37.9 | 7.6 / 14.7 ms | | |
| 4000 | 36.1 | 9.4 / 16.3 ms | 71.8 | 11.5 / 12.4 ms |
| 6000 | 36.4 | 13.6 / 20.0 ms | 73.0 | 16.9 / 18.5 ms |
| **8000** | **34.3** | 17.8 / 24.2 ms | **59.3** | 22 / 24 ms |
| 12000 | 33.9 | 27.0 / 33.6 ms | 61.4 | 32.6 / 36.4 ms |

CPU flattens from about 6-8 ms (8 vs 12 ms is inside the box's drift); latency grows linearly with
the window. 8 ms is the smallest window on the flat part: -16% whole-box CPU dense and -22% sparse
against the 2 ms it replaced, for a p95 around 20 ms against the 500 ms limit. The local rig had
said the opposite (loopback wake-ups are cheap), which is why this was not the default before the
boxes. Not swept: 7, 9, 10 ms (differences would be under the noise floor), and a 16-core box, where
each loop sees ~2,600 req/s at a million users (three times the per-loop rate here), so the knee may
sit lower than 8 ms there.

### Real boxes: accept steering and pinning

Same hour, dense: 20.2 / 40.8 (default: pinned, steered) vs 19.9-20.3 / 40.5 (`CBPF=0`) vs
20.1 / 40.3 (`PIN=0`), process / whole-box us per request. **No measurable effect** on a 4-vCPU VM
with one virtio queue: the NIC's softirq sits on one core whatever the loops do. Kept on; harmless.
16 cores with several NIC queues may differ; untested.

### Real boxes: where the process's time goes (`perf`)

Dense, io_uring, 4 ms window: 44% of the process's cycles in the kernel (io_uring submit path, TCP
send/recv, softirq run from the syscall), 42% in the server's own code (gzip and its assembly 19%,
JSON 9%, the rest parsing, Postgres decode and dispatch), 13% libc (`memcpy`, `memchr`). AppArmor's
socket hooks are about 2% of the process on every send and recv. The remaining lever inside the
process is gzip; outside it, the kernel's TCP path, which is the same for every entry.

### Real boxes: spurious retransmits

About 13% of responses in sparse runs showed a TCP timeout retransmit (`TcpExtTCPTimeouts`, no
`TcpExtTCPLostRetransmit`, no duplicate ACKs on the load box, client ACKs within the RTT). Swapping
the `fq` qdisc for `pfifo_fast` changed nothing. Diagnosed as the virtio TX completion arriving
late for lone small segments on an idle connection, so the RTO fires before the ACK is processed.
Not loss, not the client, not the server: the same for every entry on this host type.

## Where it stands

| | process us/req | whole box us/req | p95 / p99 | RSS |
|---|---:|---:|---|---:|
| box, dense, io_uring, 8 ms window (default) | **17.2** | **33.9** | 18 / 25 ms | 31 MB |
| box, dense, io_uring, 2 ms (4-round mean) | 20.4 | 40.4 | 6 / 16 ms | 29 MB |
| box, dense, epoll, 8 ms | 23.1 | 39.1 | 17 / 24 ms | 11 MB |
| box, dense, epoll, 2 ms (4-round mean) | 24.1 | 43.3 | 6 / 17 ms | 10 MB |
| box, sparse, io_uring, 8 ms (default) | **26.0** | **59.3** | 22 / 24 ms | 49 MB |
| box, sparse, io_uring, 2 ms (2-round mean) | 38.5 | 76.4 | 6 / 8 ms | 49 MB |
| box, sparse, epoll, 2 ms (2-round mean) | 39.2 | 79.3 | 5 / 8 ms | 30 MB |
| box, Go prototype, 8 ms: dense / sparse | 20.5 / 32.2 | 36.5 / 62.4 | 18 / 24, 16 / 18 ms | 25 / 63 MB |
| box, leaderboard #1 C entry (2 ms window): dense / sparse 60k | 24.3 / 47.8 | 38.5-44.7 / 91.8 | 8-10 / 16-22 ms | 5-7 MB |
| box, leaderboard #3 Rust entry (2 ms): dense / sparse 60k | 40.0 / 65.5 | 61.5 / 110.6 | 10 / 18 ms | 21-60 MB |
| local rig, T30 (31.5k req/s), io_uring / epoll | 14.3-15.6 / 14.8-15.0 | | 7 / 76, 6 / 16 ms | 50 / 44 MB |
| local rig, 150k users scoring profile, io_uring / epoll | 35-41 / 35-44 | | | 90 / 87 MB |

The two leaderboard entries were built from their folders and run on the same boxes, same hour,
same load; "sparse 60k" because they take a single `EXTRA_PORT` and the load box has two ports'
worth of source ports.

Defaults: `URING=1` (falls back to epoll by itself if the ring cannot be set up or an opcode is
missing), `PIN=1`, `CBPF=1`, `PG_SQE=0`, `BATCH_MICROS=8000`. 42/42, the protocol edge cases and the
outage check pass on both backends, locally and on the boxes.

### What the boxes answered

- **io_uring vs epoll: io_uring, by 15% in the process and 7% on the whole box**, in every one of
  four interleaved dense rounds and both sparse rounds. Loopback had shown them equal.
- **Accept steering and pinning: no measurable effect** at 4 vCPUs with one NIC queue.
- **The batching window is the lever**: 2 → 8 ms is -16% whole-box CPU dense, -22% sparse.
- **The kernel outside the process is half the bill**: 17 us in the process, 34 on the box. Every
  entry sends the same 1.39 packets per request, so that half is the same for everyone.
- **Against the leaderboard's #1 (same design, C with libpq, epoll, 2 ms window)**: level on the
  2 ms window within the box's drift, ahead by the window change and io_uring. #3 (Rust, tokio)
  costs 50% more per request.

### Not done

- **16 cores, several NIC queues**: the only configuration where steering and pinning could matter,
  and where the window knee may move. Needs bigger boxes than this was measured on.
- **Provided-buffer memory**: 8 MB per loop (1,024 x 8 KB) is fixed; a million idle connections cost
  nothing extra in user space (48 bytes each), the kernel socket is what remains.
- **Not tried**: `IORING_SETUP_SQPOLL` (needs a spare core per ring: no), zero-copy send (responses
  are 100 B-2 KB: no), registered files (1M fds: the table alone is 4 MB, and `IORING_REGISTER_FILES`
  updates per accept cost more than they save at this request size).
