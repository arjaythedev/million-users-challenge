# nelua-epoll-tbsvttr: OpenResty without the nginx

The plan was OpenResty: Lua in nginx. Rule 11 says no Nginx. So this is Lua's syntax without nginx and without a
Lua VM: [Nelua](https://nelua.io), a statically typed language that reads like Lua and compiles to C. The server is
a hand-written HTTP/1.1 server on epoll with its own pipelined Postgres client. libpq only logs in, OpenSSL does
the HMAC and ISA-L the gzip, all called directly from Nelua. No framework and no runtime.

## Stack

| Piece | Version |
|---|---|
| Compiler | Nelua 0.2.0-dev at commit `a584505` (2025-06-24, the latest), built from source by `install.sh` into `/opt/nelua`. Nelua has no versioned releases, so the commit is the pin. It generates C; gcc 13 (Ubuntu 24.04) compiles it |
| HTTP | hand-written, on `epoll` |
| Postgres | hand-written wire protocol client, pipelined. libpq 16 (Ubuntu's `libpq-dev`) only logs in (SCRAM) and prepares the statements |
| JSON | hand-written: a validating scanner for the request body and JWT parts; responses written by hand |
| JWT | hand-written HS256 check on OpenSSL 3.0's `HMAC()`, compared with `CRYPTO_memcmp` |
| Compression | ISA-L 2.31's igzip (Ubuntu's `libisal-dev`), level 0 |

`build.sh` compiles `src/main.nelua` with `nelua --release` into `build/feed`. `start.sh` runs it in the foreground.
The program is built without Nelua's garbage collector: memory is `malloc`/`free`.

## Process model

- **One worker process per CPU** (`nproc`, so the affinity mask decides; `WORKERS` overrides it), started with
  `fork()`. Each worker runs its own epoll loop on one thread and has its own Postgres connections. The parent only
  waits; workers die with it (`PR_SET_PDEATHSIG`). If a worker dies, the others keep serving.
- **Every worker opens its own listening socket per port** with `SO_REUSEPORT`, so the kernel spreads new
  connections evenly over the workers. Both ports are served by every worker.
- **Batching window:** after a loop pass that handled fewer than 32 events, the worker sleeps 2 ms (`BATCH_US`), so
  the next pass serves more requests per wake-up and per Postgres round trip. A busy worker never sleeps. This
  trades a few milliseconds of latency for CPU (the idea is from `c-epoll-unorig-username`).
- **Idle connections are never closed** by the server (the spec asks for at least 75 s).

## HTTP

- A request is parsed straight from a 64 KB read buffer shared by the worker. Only bytes left over (a partial
  request, or requests pipelined behind one that waits for Postgres) are copied into a buffer of the connection,
  freed again once used.
- Responses are written with one `send()`. What the socket doesn't take is kept until `EPOLLOUT`.
- Handled: keep-alive, pipelining (in order), `Expect: 100-continue`, `Connection: close`, HTTP/1.0. Not handled:
  chunked request bodies (400 and close).
- **gzip** for bodies of 1 KB or more when the client accepts it, which in practice means the feed: igzip level 0
  turns a 3.6 KB feed into ~1 KB in ~6 µs here, against ~10 µs for libdeflate level 1 (which compresses to 0.7 KB).

## Database

- **libpq logs in and prepares the statements, then lets go.** Every statement is SPEC.md's SQL, prepared once per
  connection. After that, the worker speaks the protocol itself on the socket: `Bind` (text parameters, binary
  results) and `Execute` per query, queued per connection and sent with **one `send()` per connection at the end of
  each loop pass**.
- **Reads and writes use separate connections.** A quarter of a worker's connections (at least one) take the writes.
  - **Reads:** one `Sync` per batch, so Postgres answers the whole batch at once. A query is done at its
    `CommandComplete`. If one fails, Postgres skips the rest of the batch, and those queries fail too.
  - **Writes:** every query has its own `Sync`, so each is its own transaction. The response waits for its
    `ReadyForQuery`, which comes after the commit.
- **256 connections in total** (`DB_POOL`), 16 per worker on 16 cores. A query goes to the first connection with
  fewer than 8 queries in flight, so a pass touches few connections, which means few `send()` calls.
- Results come back in binary: ids and `like_count` as integers, `created_at` as microseconds since 2000-01-01 UTC.
  The server turns that into ISO 8601 truncated to milliseconds, without any time zone setting.
- Like on a missing post: `foreign_key_violation` (`23503`) becomes 404 `post not found`.
- **When Postgres goes away,** the queries on that connection fail (`/health` 503, everything else 500). The worker
  reconnects on a later request, at most once a second. Reconnecting is the one blocking call, with a 2 s
  `connect_timeout`.

## Memory

An idle connection is its socket in the kernel plus a ~150-byte struct in the worker. At 125,000 open connections
the server's RSS was 48 MB.

## Results

`test/run.sh`: **passed=42 failed=0** on Ubuntu 24.04 against `postgres:18`, both on aarch64 (Docker on an Apple
Silicon Mac) and on x86_64 (the same, emulated with Rosetta). On both, `install.sh` builds Nelua from source.

Also checked by hand: `created_at` `.999999` comes out `.999Z`; gzip on the feed unzips to the plain body (on both
architectures); a single post isn't gzipped; the full API on `EXTRA_PORT`; `Expect: 100-continue`; 200 requests over
one connection; 2,000 requests 200 at a time; a connection idle for 80 s still answers; 503/500 while the database
is unreachable, then recovery.

### Local load tests (not the benchmark hardware)

Same setup as `typescript-bun-tbsvttr`, `elixir-bandit-tbsvttr` and `pony-stallion-tbsvttr`: Docker on an Apple
Silicon Mac, the server's container pinned to N cores, Postgres and `mload` on their own cores. The CPU numbers are
the whole container (cgroup), including the kernel's network work for its sockets.

**The scaled-down million:** 125,000 users on 2 cores is the box's million on 16 cores, 3 min hold.

| Server | Result | Server CPU (hold) | Server RSS |
|---|---|---:|---:|
| **Nelua** | **pass**: p95 6.7 ms, p99 13.1 ms, 0 errors | 0.23 cores | 48 MB |
| Bun | pass: p95 3.9 ms | ~0.45 cores | 167 MB |
| Node example | pass: p95 3.2 ms | 0.84 cores | 1,011 MB |
| Pony | fail: p95 214 ms, p99 1.63 s | 1.80 cores | 1,678 MB |
| Elixir | fail: p95 788 ms, p99 2.96 s | 1.38 cores | 3,972 MB |

The one bad 5-second window of the Nelua run (p95 280 ms at ~75 s) came when `mload` reached its memory limit and
collected its own garbage. The other runs show the same window.

**The rate per core of a million on 4 cores:** one core, 20,000 users at `-gzip -think-scale 0.2`, ~10,500 req/s.
What each step bought:

| Version | p95 | User | System | Total |
|---|---:|---:|---:|---:|
| libpq, one query at a time per connection | 0.6 ms | 0.135 | 0.289 | 0.424 cores |
| own protocol client, pipelined | 0.5 ms | 0.121 | 0.238 | 0.359 cores |
| + one `Sync` per read batch, igzip | 0.6 ms | 0.092 | 0.227 | 0.319 cores |
| + 2 ms batching window | 6.4 ms | 0.079 | 0.159 | **0.239 cores** |

**On the box:** at a million users on 4 cores, each worker serves ~10,500 req/s, the rate of the last table. That
was 0.24 of a core here; a box core is about 1.7× slower (measured with the Node example), so ~0.4 box cores per
worker, ~1.6 of the 4 cores. Server memory stays well under 1 GB, plus the kernel's socket buffers. I expect it to
hold the million on 4 cores.
