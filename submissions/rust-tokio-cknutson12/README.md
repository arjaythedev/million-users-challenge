# rust-tokio-cknutson12: thread-per-core tokio, a tiny HTTP/1.1 layer, pipelined tokio-postgres

The feed API from `SPEC.md`. At a million users the work is small (~42,000 req/s, 5% writes). What is big is
the **million open keep-alive connections**. On a 32 GB box anything that keeps a buffer per idle connection
runs out of memory: the axum example uses ~28 KB per idle connection, which is ~28 GB at a million. So this
server is built so that an idle connection costs as little as possible, and everything else is kept plain.

## Stack

| Piece | Version |
|---|---|
| Rust | 1.94.0 (rustup in `/opt/twelve-rust`, like the Rust example), edition 2021 |
| tokio | 1.54.1, `rt` + `net` + `time` only: one current-thread runtime per core |
| tokio-postgres | 0.7.18 (postgres-protocol 0.6.12), no TLS |
| httparse | 1.10.1 (request line + headers) |
| libdeflater | 1.26.1 (libdeflate, compiled from source by the build) for gzip |
| hmac / sha2 / base64 | 0.12.1 / 0.10.9 / 0.22.1 for HS256 |
| serde_json | 1.0.154 (parsing the JWT and the POST body only; responses are written by hand) |
| socket2 | 0.6.5 (`SO_REUSEPORT` listeners) |
| Postgres server | 18 (separate box) |

Every version is pinned in `Cargo.lock` (`cargo build --locked`).

## Design

**Thread-per-core, nothing shared.** `available_parallelism()` threads (it follows the CPU affinity mask, so
4/8/16 cores need no setting). Each thread runs its own current-thread tokio runtime, binds its own
`SO_REUSEPORT` listener on both ports (the kernel spreads new connections over the threads), and opens its own
Postgres connections. No work stealing, no locks or atomics on the request path, no cross-thread wakeups.

**An idle connection owns no buffer.** Each connection is a `spawn_local` task that waits for readability,
then reads into a 64 KB per-thread scratch buffer. Requests are parsed straight out of it, and only what the
handlers need is copied out: the route, the verified JWT user, and the body of `POST /posts`. Only an
incomplete request (rare) gets copied into a per-connection `Vec`, which is freed as soon as it's used. The
handler that awaits Postgres is a separately boxed future, so the connection future stays small, and small is
what every idle connection pays. Measured with `mload`: **~1.2 KB of server RSS per open connection**
(10.6 MB idle, 34.5 MB at 20,000 connections, 184 MB at 150,000). The rest is the kernel's socket.

**Postgres: few connections, pipelined, reads apart from writes.** tokio-postgres pipelines concurrent
queries on one connection, so each thread needs only a few. By default there are 64 read and 64 write
connections in total, split evenly over the threads (16 cores: 4 + 4 per thread; 4 cores: 16 + 16). Each query
goes to the thread's open connection with the fewest queries in flight. Writes have their own connections, so
a `/feed` read never waits in a pipeline behind an INSERT's commit (WAL flush). Every statement is prepared
once per connection. A connection that drops reconnects in the background (50 ms, then up to 1 s backoff)
while the others carry the load.

**The SQL is SPEC.md's, one statement per request:** the post `SELECT` (with `LIMIT 20` for the feed, or
`WHERE p.id = $1`), `INSERT ... RETURNING id, created_at` and `INSERT ... ON CONFLICT DO NOTHING`. Each request
runs as one autocommit statement. There is no batching across requests, and no caching of any kind: every
request reads Postgres, and every token's HS256 signature, `alg` and `exp` (and `nbf` if present) are
verified on every request.

**Responses are written by hand**: compact JSON in the spec's key order. `created_at` is decoded from the
binary `TIMESTAMPTZ` (microseconds since 2000-01-01) and formatted in UTC, truncated to milliseconds.
Strings escape `"`, `\` and control characters; non-ASCII is copied as-is.

**gzip** (libdeflate, level 1, a per-thread compressor) for bodies of 512 bytes or more when the client
accepts it: `/feed` goes from 5.1 KB to 1.9 KB. Single posts (~300 bytes) and the small replies go out
uncompressed, because gzip saves almost nothing at that size.

**HTTP/1.1 keep-alive with no idle timeout** (the spec asks for at least 75 s), `TCP_NODELAY`, listen
backlog 65535, `Content-Length` bodies up to 64 KB, pipelined requests answered in order, `Expect:
100-continue`, `Connection: close` and HTTP/1.0 keep-alive. Chunked request bodies get a 400 (nothing sends them).

## Settings

All optional. The defaults are what gets benchmarked.

| Env | Default | |
|---|---|---|
| `THREADS` | `available_parallelism()` | worker threads |
| `DB_READ_CONNS` | 64 | Postgres connections for reads, in total (rounded up to a multiple of the thread count) |
| `DB_WRITE_CONNS` | 64 | Postgres connections for writes, in total; read + write must stay ≤ 512 |
| `GZIP_LEVEL` | 1 | libdeflate level 1–12 |
| `GZIP_MIN` | 512 | smallest body that is gzipped |

## Running

```bash
sudo ./install.sh   # build-essential + curl if missing, Rust 1.94.0 in /opt/twelve-rust if missing
./build.sh          # cargo build --release --locked, target-cpu=native
DATABASE_URL=postgres://user:pass@host:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 ./start.sh
```

## Measured (not on the benchmark boxes)

On a laptop (Docker, 12 arm64 cores shared by the server, Postgres 18 and `mload`, all in one 8 GB VM):

- `test/run.sh`: `passed=42 failed=0` (built and run on x86_64 Ubuntu 24.04 against Postgres 18).
- `mload -users 50000 -ramp 40s -hold 30s -gzip -think-scale 2.5` over both ports:
  `RESULT users=50000 req/s=1812 p95=3.1ms p99=45.3ms err=0.000% pass=true`, server RSS ~70 MB.
- Past ~80,000 users the load generator itself runs out of CPU and memory in that VM. Its reported latency
  climbs, while a separate `curl` timing `/feed` and `/posts/:id` once a second through the same run stays
  under 50 ms. So that is the laptop's limit, not the server's. The server used under one core throughout,
  and `perf` shows most of its time in the kernel's loopback TCP and Docker's bridge, not in this code.
- ~7 syscalls per request: one wakeup, a read and an `EAGAIN` read on the client socket, the pipelined
  query's send and receive on the Postgres socket, and one send for the response.
