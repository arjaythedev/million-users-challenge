# nelua-epoll-tbsvttr: OpenResty without the nginx

The plan was OpenResty: Lua in nginx. Rule 11 says no Nginx. So this is Lua's syntax without nginx and without a
Lua VM: [Nelua](https://nelua.io), a statically typed language that reads like Lua and compiles to C. The server is
a hand-written HTTP/1.1 server on epoll, libpq in its asynchronous mode for Postgres, OpenSSL for HMAC and
libdeflate for gzip, all called directly from Nelua. No framework and no runtime.

## Stack

| Piece | Version |
|---|---|
| Compiler | Nelua 0.2.0-dev at commit `a584505` (2025-06-24, the latest), built from source by `install.sh` into `/opt/nelua`. Nelua has no versioned releases, so the commit is the pin. It generates C; gcc 13 (Ubuntu 24.04) compiles it |
| HTTP | hand-written, on `epoll` |
| Postgres driver | libpq 16 (Ubuntu's `libpq-dev`), asynchronous: `PQsendQueryPrepared`, `PQconsumeInput`, `PQgetResult` |
| JSON | hand-written: a validating scanner for the request body and JWT parts; responses written by hand |
| JWT | hand-written HS256 check on OpenSSL 3.0's `HMAC()`, compared with `CRYPTO_memcmp` |
| Compression | libdeflate 1.19 (Ubuntu's `libdeflate-dev`), gzip at level 1 |

`build.sh` compiles `src/main.nelua` with `nelua --release` into `build/feed`. `start.sh` runs it in the foreground.
The program is built without Nelua's garbage collector: memory is `malloc`/`free`.

## Process model

- **One worker process per CPU** (`nproc`, so the affinity mask decides; `WORKERS` overrides it), started with
  `fork()`. Each worker runs its own epoll loop on one thread and has its own Postgres connections. The parent only
  waits; workers die with it (`PR_SET_PDEATHSIG`). If a worker dies, the others keep serving.
- **Every worker opens its own listening socket per port** with `SO_REUSEPORT`, so the kernel spreads new
  connections evenly over the workers. Both ports are served by every worker.
- **Idle connections are never closed** by the server (the spec asks for at least 75 s).

## HTTP

- A request is parsed straight from a 64 KB read buffer shared by the worker. Only bytes left over (a partial
  request, or requests pipelined behind one that waits for Postgres) are copied into a buffer of the connection,
  freed again once used.
- Responses are written with one `send()`. What the socket doesn't take is kept until `EPOLLOUT`.
- Handled: keep-alive, pipelining (in order), `Expect: 100-continue`, `Connection: close`, HTTP/1.0. Not handled:
  chunked request bodies (400 and close).

## Database

- **256 Postgres connections in total** (`DB_POOL`), split evenly: 16 per worker on 16 cores. One query runs on each
  connection at a time; when all of a worker's are busy, requests wait in a FIFO queue.
- Every statement is SPEC.md's SQL, prepared once per connection, one per request, in autocommit. Results come
  back in binary: ids and `like_count` as integers, `created_at` as microseconds since 2000-01-01 UTC, which is
  turned into ISO 8601 and truncated to milliseconds without any time zone setting.
- Like on a missing post: `foreign_key_violation` (`23503`) becomes 404 `post not found`.
- **When Postgres goes away,** the connection's query fails (`/health` 503 with libpq's error, everything else 500)
  and the worker reconnects on a later request, at most once a second. Reconnecting is the one blocking call, with a
  2 s `connect_timeout`.

## Memory

An idle connection is its socket in the kernel plus a ~150-byte struct in the worker. At 125,000 open connections
the server's RSS was 46 MB.

## Results

`test/run.sh`: **passed=42 failed=0** on Ubuntu 24.04 against `postgres:18`, both on aarch64 (Docker on an Apple
Silicon Mac) and on x86_64 (the same, emulated with Rosetta). On both, `install.sh` builds Nelua from source.

Also checked by hand: `created_at` `.999999` comes out `.999Z`; gzip on the feed (1,840 bytes) unzips to the plain
body; a single post isn't gzipped; the full API on `EXTRA_PORT`; `Expect: 100-continue`; 200 requests over one
connection; 2,000 requests 200 at a time; a connection idle for 80 s still answers; 503/500 while the database is
unreachable, then recovery.

### Local load tests (not the benchmark hardware)

Same setup as `typescript-bun-tbsvttr`, `elixir-bandit-tbsvttr` and `pony-stallion-tbsvttr`: Docker on an Apple
Silicon Mac, the server's container pinned to N cores, Postgres and `mload` on their own cores. 125,000 users on
2 cores is the box's million on 16 cores, scaled down.

| Server | Cores | Load | Result | Server CPU (hold) | Server RSS |
|---|---:|---|---|---:|---:|
| **Nelua** | 1 | 10,000 users, `-gzip -think-scale 0.5` (~2,100 req/s) | p95 0.4 ms | 0.10 cores | 20 MB |
| **Nelua** | 2 | 125,000 users, `-gzip -think-scale 2.5`, 3 min hold | **pass**: p95 0.5 ms, p99 2.0 ms, 0 errors | 0.29 cores | 46 MB |
| Bun | 2 | the same | pass: p95 3.9 ms | ~0.45 cores | 167 MB |
| Node example | 2 | the same | pass: p95 3.2 ms | 0.84 cores | 1,011 MB |
| Pony | 2 | the same | fail: p95 214 ms, p99 1.63 s | 1.80 cores | 1,678 MB |
| Elixir | 2 | the same | fail: p95 788 ms, p99 2.96 s | 1.38 cores | 3,972 MB |

- The CPU column is the whole container (cgroup), including the kernel's network work for those sockets.
- **The one bad stretch** of the 125,000 run was at ~200 s (p95 202 ms for 5 s), when `mload` collected its own
  garbage at its memory limit. The other submissions' runs show the same window.
- **On the box:** ×8 for a million users is ~2.3 cores' worth of this machine, and a box core is about 1.7× slower
  (measured with the Node example), so ~4 of the 16 cores. Server memory would be well under 1 GB, plus the kernel's
  socket buffers. I expect it to hold the million; the network and Postgres are more likely limits than this server.
