# rust-pulse: one io_uring loop per core, hand-written Postgres client, HTTP and gzip

| | |
|---|---|
| Language | Rust 1.99.0 (edition 2021), installed by `install.sh` with `rustup-init` 1.28.2 (SHA-256 checked) into `/opt/pulse-rust` |
| Framework / IO | none: raw `io_uring` system calls (`src/uring.rs`), with an `epoll` loop as fallback |
| Driver | none: `src/pg.rs` speaks the Postgres v3 protocol (named prepared statements, Bind/Execute/Sync, binary results), cleartext or SCRAM-SHA-256 login, no TLS |
| Front | direct, on `PORT` and `EXTRA_PORT` (no Nginx) |
| Crates | `libc` 0.2.191 only, pinned in `Cargo.toml` (`=0.2.191`) and `Cargo.lock` (`cargo build --locked`) |

Needs Linux 6.1 or newer for the io_uring features it uses (Ubuntu 24.04 ships 6.8). On an older kernel, or with
`PULSE_EPOLL=1`, it runs the epoll loop instead.

## Running

```bash
sudo bash install.sh   # curl, gcc, Rust 1.99.0 (idempotent)
bash build.sh          # target/release/pulse: fat LTO, codegen-units=1, panic=abort
DATABASE_URL=postgres://feed:pw@db:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

On x86_64, `build.sh` compiles for Cascade Lake (the Xeon Platinum 8260 of the app box) without AVX-512, so the
code uses AVX2 and PCLMULQDQ. On other CPUs it uses `target-cpu=native`.

## Design

- **One worker thread per CPU in the affinity mask**, so the server follows the 16-, 8-, 4- or 2-core limit by
  itself. Each worker owns its own `SO_REUSEPORT` listeners on both ports, its own io_uring, its own Postgres
  connections and its own buffers. Workers share nothing except the read-only configuration. Each listener sets
  `TCP_NODELAY`, `TCP_DEFER_ACCEPT`, `TCP_THIN_LINEAR_TIMEOUTS` and `TCP_QUICKACK=0`. New connections are shared
  between the listeners by the kernel's `SO_REUSEPORT` hash. The listeners do not set `SO_INCOMING_CPU`: that
  option gives a new connection to the listener on the CPU that processed its SYN, so a skewed receive path (one
  NIC queue, an RPS mask that names offline CPUs, or two workers on one CPU) would give one worker most
  connections.
- **io_uring**: one ring per worker with `SINGLE_ISSUER`, `DEFER_TASKRUN` and `SUBMIT_ALL`. One multishot accept
  per listener puts new sockets straight into the ring's registered file table (up to 1,048,576 per worker).
  Each connection has one multishot receive that takes 2 KB buffers from a provided-buffer ring, so an idle
  keep-alive connection holds no receive buffer. After a pass that did work, the worker submits everything it
  queued and waits once for 512 completions or 3 ms, whichever comes first. Requests that arrive in that time
  share one wake-up and one Postgres round trip. Most sends complete inside the submit call and post their
  completions at once, so a smaller minimum (128) let a pass that answered many requests end its wait immediately.
  Below saturation, 3 ms and 512 completions used 12-17% less CPU per request than 1.5 ms and 128, and about 16%
  less Postgres CPU; at saturation, where every pass is full anyway, throughput was the same.
- **Postgres**: 32 read and 16 write connections in total, split across the workers. Each connection runs session-level
  `SET plan_cache_mode = 'force_generic_plan'`, `SET max_parallel_workers_per_gather = 0`, and
  `SET client_min_messages = 'error'` (rule 2), and prepares the 5 statements once at startup. Each statement is the SQL
  from `SPEC.md`.
  - Reads: up to 128 queued reads go out as Bind/Execute pairs under one `Sync`, on the read connection with the
    fewest reads in flight. A read connection that has owed an answer for 10 seconds is closed and its reads are
    sent again on another connection.
  - Writes: group commit. The queued posts and likes are sorted, so concurrent groups take row locks in the same
    order. They go out under one `Sync`, which makes them one implicit transaction and one commit. Each response
    is sent only after that group's `ReadyForQuery`, so every write is committed before its response. If one
    write in a group fails (a like on a post that does not exist, SQLSTATE `23503`), that request gets its 404
    and the other writes are run again, one per `Sync`.
- **HTTP**: requests are parsed in place in the receive buffer. Pipelined requests are answered in order.
  Responses are built in one buffer with the headers in front of the body, and sent with one send.
- **JWT**: HS256 with the HMAC inner and outer SHA-256 states computed once from `JWT_SECRET`, so each check
  hashes only the token. Every request checks the signature and `exp`. Nothing is remembered between requests.
- **JSON**: a strict parser for the `{"body": ...}` request. Trimming and the 500-character limit follow
  JavaScript's `trim()` and string length, so the results match the reference servers. Output escaping uses SIMD
  to skip runs of bytes that need no escaping. Timestamps are written with millisecond precision.
- **gzip** (`src/gzip.rs`): a single-block deflate with a Huffman table trained at startup on generated feed
  JSON, so no table has to be built per response. The match finder uses a 4 KB hash table that is never cleared:
  each response moves a base offset forward, which makes old entries look out of range. CRC-32 uses the CPU's
  CRC instructions (PCLMULQDQ folding on x86_64, the CRC32 instructions on arm64). `PULSE_GZ_FIXED=1` uses the
  standard fixed Huffman table instead, for comparison.
- **Workers** run at `SCHED_IDLE` priority by default (`PULSE_SCHED_IDLE=0` turns this off). When a worker and a
  kernel thread such as `ksoftirqd` (which processes network packets under load) want the same CPU, the kernel
  thread runs first.

## Settings

Only the variables from `SPEC.md` are needed. The others are for experiments and default to the values above.

| Variable | Default | Meaning |
|---|---|---|
| `PULSE_THREADS` | CPUs in the affinity mask | worker count |
| `PULSE_PG_READ`, `PULSE_PG_WRITE` | 32, 16 | total Postgres connections (at most 500 in total) |
| `PULSE_READ_BATCH`, `PULSE_WRITE_BATCH` | 128, 512 | most requests under one `Sync` |
| `PULSE_BATCH_US`, `PULSE_MIN_COMPLETE` | 3000, 512 | how long, and for how many completions, a worker waits |
| `PULSE_DEFER_ACCEPT` | 2 | `TCP_DEFER_ACCEPT` seconds on the listeners (0 turns it off) |
| `PULSE_EPOLL` | 0 | 1 runs the epoll loop instead of io_uring |
| `PULSE_STATS` | 0 | 1 prints per-worker counters every 10 seconds |

## Tests

`cargo test --release` runs the unit tests: HTTP parsing, JSON, JWT, SHA-256 vectors, CRC-32 against the table
version, and gzip output checked with the `gzip` command (one round trip per input and 10,000 random inputs).
`bash test/run.sh submissions/rust-pulse-tachytelicdetonation` passes 42/42 with io_uring and with `PULSE_EPOLL=1`.
