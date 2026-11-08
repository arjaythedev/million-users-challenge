# rust-tokio-dadujex: a small HTTP/1.1 server on tokio + tokio-postgres

The feed API from [`SPEC.md`](../../SPEC.md) in Rust. No web framework: a ~250-line HTTP/1.1 loop on tokio,
built around one number. A million keep-alive connections at 16-28 KB each (hyper's buffers, in the axum
example) is 16-28 GB before any request is served. Here an idle connection costs **~1 KB** (measured below).

## Stack

| Piece | Version |
|---|---|
| Rust | 1.94.0 stable (rustup in `/opt/twelve-rust`, like `examples/rust`), edition 2024 |
| Async runtime | tokio 1.54.1, `rt` + `net` + `time`: one **current-thread** runtime per worker thread |
| HTTP | hand-written HTTP/1.1 on `tokio::net::TcpStream` (`src/http.rs`), httparse 1.10.1 for the request head |
| Postgres driver | tokio-postgres 0.7.18 (pure Rust, no TLS), prepared statements, pipelined queries |
| gzip | libdeflater 1.26.1 (libdeflate 1.26, built from source by its build script) |
| JWT | hmac 0.13.0 + sha2 0.11.1 (HS256), base64 0.22.1, serde_json 1.0.154 for the header and payload |
| JSON | written by hand (`src/json.rs`); serde_json only parses the `POST /posts` body |
| Allocator | mimalloc 0.1.52 (mimalloc 2.3) |

Exact versions are pinned in `Cargo.lock` (`cargo build --locked`). `install.sh` installs `build-essential` (the
linker, and cc for libdeflate and mimalloc) and Rust 1.94.0. The binary links only glibc.

## Design, and why

**Thread per core, nothing shared.** `available_parallelism()` worker threads (it follows the CPU affinity mask and
cgroup quota, so 16, 8 or 4 on the benchmark box). Each thread runs its own current-thread tokio runtime with:

- its own listeners on `PORT` and `EXTRA_PORT`, opened with `SO_REUSEPORT`, so the kernel spreads new connections
  evenly over the threads. No accept thread, no work stealing, no cross-thread wakeups.
- its own Postgres connections and its own scratch buffers. State is `Rc`/`Cell`, no locks or atomics on the request path.

**~1 KB per idle connection.** A connection is one tokio task plus its socket registration. While it waits for the
next request it holds **no buffer at all**:

- the request is read through a 4 KB stack buffer, and only the bytes that arrived are kept. That buffer is freed as
  soon as the request is consumed (pipelined leftovers stay until they are served);
- the response is built in the thread's scratch buffers and written with one `write` that the socket takes whole.
  Only if the socket buffer is full is the rest copied out to wait for writability;
- the database part of a request is a boxed future, so its state exists only while the request is in flight and
  isn't part of the connection's idle task.

No idle timeout (keep-alive connections stay open as long as the client keeps them), no `SO_KEEPALIVE` probes
(the Go example's fix), `TCP_NODELAY` on (a gzipped feed is 2 segments; Nagle must not hold back the second one).

**Postgres: pipelined, no checkout.** Per thread, `DB_READ_CONNS / threads` read connections and
`DB_WRITE_CONNS / threads` write connections (totals 128 + 32 = 160 by default, at most 512 enforced at startup).
Each has the SPEC's five statements prepared once (feed, post by id, insert post, insert like, `SELECT 1`). A request
takes the first idle connection (or the least busy) and sends its query immediately; tokio-postgres pipelines
concurrent queries on one connection, so there is no pool to wait for and no `test_before_acquire` round trip. Reads
and writes use separate connections: a pipelined read never queues behind an INSERT waiting for its WAL flush. A
dropped connection is reopened by its own task (100 ms to 5 s backoff), and writes fall back to a read connection while
every write connection is down.

Write capacity is *write connections / WAL flush time*: each commit on a connection waits for its own flush. On the
test VPS below (replicated storage, ~4 ms per flush), 8 write connections queued up at ~1,900 writes/s and stalled for
up to a second; the default 32 keep up, and 64 changed nothing (p95 1.7 ms, p99 21.7 ms against 1.8 / 18.0 ms at 23k
req/s). That's why writes have connections of their own and take an idle one whenever there is one.

The SQL is SPEC.md's, verbatim, one statement per request, autocommit (`synchronous_commit` untouched).
`like_count` is the correlated `count(*)` in the same SELECT. A like on a missing post fails the foreign key
(SQLSTATE 23503) -> 404 `post not found`; `already_liked` comes from the rows-affected count.

**gzip only where it pays.** Bodies of 1 KB or more are gzipped when the client sends `Accept-Encoding: gzip`. In
practice that's only the feed: ~5.3 KB -> ~1.8 KB with libdeflate level 1, which is 2 TCP segments instead of 4. A post or
a like is a few hundred bytes, where gzip saves nothing and costs CPU. libdeflate compresses a whole buffer in one call
(no streaming state per response), and level 1 is the fastest level: on 4 cores, CPU matters more than ~10% of feed bytes.

**Everything else per request, nothing remembered.** HS256 signature (constant-time compare), `alg`, and `exp` are
checked on every authenticated request; the HMAC key schedule is computed once at startup (a keyed `Hmac<Sha256>`
cloned per verification), which is key setup, not a remembered verification. No response or query-result caches.
`created_at` is decoded from Postgres's binary TIMESTAMPTZ (microseconds since 2000-01-01) and formatted in UTC,
truncated to milliseconds.

**Behaviour details.** HTTP/1.1 keep-alive and pipelining, HTTP/1.0 (`Connection: keep-alive` honoured),
`Connection: close`, `Expect: 100-continue`, query strings ignored for routing. Request heads up to 16 KB and bodies up
to 64 KB (`Content-Length` only; chunked request bodies get 411). Unknown paths and wrong methods: 404 `not found`.
A 500 is logged to stderr. A worker thread that dies takes the process down (the box doesn't restart it, but serving
on fewer threads would be worse to debug).

## Settings

Only the SPEC's variables are needed. Optional ones, for experiments (the defaults are what gets benchmarked):

| Variable | Default | |
|---|---|---|
| `THREADS` | cores available to the process | worker threads |
| `DB_READ_CONNS` | 128 | read connections, total over all threads (rounded up to a multiple of the thread count) |
| `DB_WRITE_CONNS` | 32 | write connections, total |
| `GZIP_LEVEL` | 1 | libdeflate level 1-12, 0 turns gzip off |

## Running

```bash
sudo bash install.sh   # build-essential + curl if missing, Rust 1.94.0 in /opt/twelve-rust if missing
bash build.sh          # cargo build --release --locked, target-cpu=native
DATABASE_URL=postgres://user:pass@host:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

`start.sh` raises the soft open-files limit to the hard limit (a no-op under systemd's `LimitNOFILE`).

## Measured

### One Synteq VPS (16 vCPU shared, 8 GB, Dallas, Ubuntu 24.04)

Everything on one box, set up with `box/setup-app-box.sh`, this folder's `install.sh` / `build.sh` and the challenge
systemd unit, plus Postgres 18 with `box/postgresql.conf` (memory settings scaled down to 8 GB). The server ran on its
own cores (systemd `AllowedCPUs`, so it started 8 or 4 worker threads); Postgres and the load generators shared the
rest, over loopback. Each run: a fresh database, 60 s warm-up, ramp, 120 s hold, `-gzip`. Server CPU and memory were
measured over 60 s of the hold.

| Load | Server cores | req/s | p95 | p99 | Errors | Server CPU | Server memory |
|---|---|---:|---:|---:|---:|---|---|
| 100,000 `mload` users, `-think-scale 2.5` (scoring model) | 8 | 4,141 | 1.1 ms | 2.7 ms | 0 | 72 µs/request | 1,219 B/connection |
| 40,000 users, `-think-scale 0.25` | 8 | 15,374 | 1.7 ms | 8.6 ms | 0 | 79 µs/request, cores 85% idle | 1,101 B/connection |
| 60,000 users, `-think-scale 0.25` | **4** | **23,057** | 1.8 ms | 18.0 ms | 0 | 66 µs/request, cores 62% idle | 1,179 B/connection |
| 400,000 idle connections + 20,000 users | 8 | 838 | 1.2 ms | 2.9 ms | 0 | 7% of one core | 1,263 B/connection (544 MB) |
| **500,000 idle connections** + 20,000 users | 8 | 838 | 1.3 ms | 3.0 ms | 0 | 7% of one core | **1,266 B/connection (666 MB)** |

- The idle connections came from a small Go client that opens connections at 5,000/s, sends one request on each, then
  holds them idle with Go's default TCP keep-alive (like `mload`'s users). Every 30 s it sent a request on 2,000
  random idle connections: 10,000 of 10,000 answered, after up to 4 minutes idle. 520k connections over 8 threads is
  65k per thread, more than a million over 16 threads (62.5k).
- Memory per connection stays flat from 40k to 520k connections: ~1.3 GB at a million, plus the kernel's socket memory.
- CPU: 66-79 µs per request (user ~45%, kernel ~55%). A million users' ~42,000 req/s is ~3 cores over loopback; with a
  real NIC, plus a million idle connections' keep-alive probes, expect ~4-5.
- The 18 ms p99 at 23k req/s was the same on every endpoint (reads included, p95 under 0.5 ms) and came in bursts,
  while the server's cores were 62% idle. Most likely it's contention between Postgres's backends and the load
  generator on the shared cores, plus the VPS's replicated storage, rather than the server; this setup can't separate
  them.
- Tried and not kept: letting a read connection take up to 8 queries before the next one is used, so that bursts
  share fewer, bigger writes to Postgres. Two alternating A/B pairs at 23k req/s on 4 cores: 60 / 60 µs per request
  against 61 / 62 µs, read p95 +0.3 ms, the same p99. Inside the run-to-run noise, so the simpler rule (the first idle
  connection) stays.

### Laptop (Docker Desktop, 8-vCPU VM)

`test/run.sh` in a clean Ubuntu 24.04 container with Postgres 18 in another: `passed=42 failed=0`, also on
`EXTRA_PORT`. `GET /feed` with gzip: 5,303 -> 1,833 bytes. A connection idle for 85 s served
its next request. Pipelined requests on one connection are answered in order. `Expect: 100-continue` with a 2.4 KB
body: 20 ms, no 1 s client wait. Postgres restarted under the server: `/health` is 200 again within ~3 s and every
connection reconnects on its own.
