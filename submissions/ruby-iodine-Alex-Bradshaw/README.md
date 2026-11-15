# ruby-iodine-Alex-Bradshaw: Ruby 4.0 (YJIT) + Iodine + pg

A Ruby entry. No Rails, no Rack framework: Iodine's C reactor holds the sockets and a thin NeoRack handler does the
per-request work.

## Stack

| | |
|---|---|
| Language | Ruby 4.0.9, built from the official tarball with YJIT (`install.sh`) |
| Server | [Iodine](https://github.com/boazsegev/iodine) 0.8.3 (facil.io C core, epoll), built from its `v0.8.3` tag with a 10-line `SO_REUSEPORT` patch (`iodine-so_reuseport.patch`, below) |
| API | NeoRack (`on_http(e)`), not Rack: no env Hash is built per request |
| Postgres driver | [pg](https://github.com/ged/ruby-pg) 1.6.3, compiled from source against the system libpq |
| JSON | stdlib `json` for parsing request bodies and escaping strings that need it; responses are built by hand |
| JWT | hand-rolled HS256 check with `OpenSSL::HMAC` + `OpenSSL.fixed_length_secure_compare` (no `jwt` gem) |
| gzip | Iodine's dynamic deflate, in C |

```bash
sudo bash install.sh   # Ruby 4.0.9 + YJIT into /opt/ruby-4.0.9, plus libpq-dev etc.
bash build.sh          # patched Iodine + pinned gems into ./vendor/bundle
DATABASE_URL=postgres://user:pass@host:5432/feed JWT_SECRET=million-users-challenge HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

Optional env: `WORKERS` (default: one per core in the CPU affinity mask), `THREADS` (Ruby threads per worker,
default 6), `REUSEPORT` (default 1; 0 = Iodine's own cluster mode with one shared listener), `RUBY_PREFIX` (default `/opt/ruby-4.0.9`), `MALLOC_ARENA_MAX` (default 2).

## Design

The hard part of this challenge is not the request rate (about 42k req/s at a million users) but holding a million
mostly idle keep-alive connections in 32 GB. So the split is:

- **Sockets live in C.** Each Iodine worker has a C reactor thread that accepts, parses HTTP/1.1, handles keep-alive
  and writes responses outside the GVL. An idle connection costs facil.io's per-connection state and kernel socket
  buffers, never a Ruby object, fiber or thread.
- **Ruby only sees live requests.** Each worker runs `THREADS` Ruby threads. At ~1 ms per request round trip to
  Postgres, Little's law says only a few dozen requests are in flight across the whole box, so plain blocking
  threads are enough. `pg` releases the GVL while it waits on the network, so one worker's threads overlap their
  queries.
- **One worker per core**, one Postgres connection per Ruby thread, opened lazily on the thread's first request
  (after the fork). 16 cores x 6 threads = 96 connections, well under the 512 cap.

### One listener per worker (`SO_REUSEPORT`)

Iodine's cluster mode opens one listening socket in the master and every worker accepts from it. The kernel wakes
the most recently waiting worker first, so connections pile up on a few workers. Measured on a 16-vCPU box (server on
6 cores, 6 workers): **5,861 to 25,998 connections per worker (4.4x)**, and worker CPU followed it exactly (23% to
105%). Each worker's Ruby code is limited to one core by the GVL, so the busiest worker saturates while the box
still has CPU to spare; the spread grows with the worker count (7.7x with 18 workers).

So `start.sh` runs a small supervisor (`server.rb`, before Iodine is loaded) that spawns `WORKERS` independent
single-process Iodine servers. Each opens its own listeners on both ports with `SO_REUSEPORT`, and the kernel hashes
new connections evenly across them. Iodine has no option for `SO_REUSEPORT` on TCP listeners, so `build.sh` clones
Iodine's `v0.8.3` tag (commit checked), applies `iodine-so_reuseport.patch` (sets `SO_REUSEPORT` next to the existing
`SO_REUSEADDR` when the `IODINE_REUSEPORT` env var is set), builds that gem and installs it into `vendor/bundle`.
Workers are spawned rather than forked because Iodine's C core is not safe to fork once loaded. If any worker exits,
the supervisor stops the rest and exits. `REUSEPORT=0` falls back to Iodine's own cluster mode.

### Per-request work

- Routing is a `case` on the path plus `byteslice`, no router.
- The five statements from SPEC.md are prepared once per connection (`exec_prepared`), verbatim, one per request.
- Results come back from libpq as text, so ids and `like_count` are copied straight into the response; only `body`
  and `author` go through a JSON escape, and only when they contain `"`, `\` or a control character.
- `created_at` is read in a UTC session (`SET TimeZone = 'UTC'`, session-level) and reformatted with string slicing,
  truncated (not rounded) to milliseconds.
- The JWT check runs on every request: base64url decode, HMAC-SHA256, constant-time compare, `alg` must be HS256,
  `exp` must be present and in the future. The standard `{"alg":"HS256","typ":"JWT"}` header segment is compared as a
  string instead of being decoded (any other header is decoded and checked).
- Error responses are frozen constant strings.

### Rules check

- No caching across requests: every request queries Postgres, and nothing about tokens is remembered.
- Writes are plain autocommit statements, committed before the response is written.
- No Nginx: every worker listens on `PORT` and `EXTRA_PORT` directly (its own `SO_REUSEPORT` sockets).
- Keep-alive timeout is 120 s (SPEC: at least 75 s).

## Ideas not done yet

- Tune `THREADS` (and `WORKERS` above the core count) with `bench/mload`.
- CPU per request: the 4-core run was CPU-bound (73% app CPU), so profile the Ruby side (`stackprof`).
- Batch writes (likes, posts) from several threads into one transaction (allowed by the rules).
- GC tuning (`RUBY_GC_HEAP_*`), and a cheaper non-exception path for auth/validation errors.

## Results

### Official, first version (PR #70, shared listener)

| Cores | Users held 5 min | req/s | p95 | p99 | App CPU |
|---:|---:|---:|---:|---:|---:|
| 16 | 550,000 | 19,732 | 106.6 ms | 220.5 ms | 38% |
| 8 | 510,000 | 18,494 | 78.3 ms | 328.3 ms | 67% |
| 4 | 235,000 | 9,327 | 53.7 ms | 377.3 ms | 73% |

16 cores barely beat 8, with most of the CPU idle: the uneven connection spread described above.

### Shared listener vs `SO_REUSEPORT` (my own 16-vCPU VPS, rough)

Server pinned to cores 0-5 (6 workers x 6 threads), Postgres 16 on cores 6-13, `mload -gzip` on 14-15, all on one
box (so Postgres is local and the clients are on loopback). 100,000 users, fresh `feed_seed` copy per run, each
configuration run twice:

| think-scale (~req/s) | Mode | Connections per worker | Worker CPU | p95 | p99 |
|---|---|---|---|---:|---:|
| 0.42 (~21k) | shared | 12,625 – 20,563 | 68 – 100% | 413 / 236 ms | 535 / 381 ms |
| 0.42 (~21k) | `SO_REUSEPORT` | 16,465 – 17,006 | 89 – 92% | 70 / 93 ms | 256 / 232 ms |
| 0.5 (~18k) | shared | 9,781 – 23,602 | 43 – 112% | 36 / 11 ms | 214 / 110 ms |
| 0.5 (~18k) | `SO_REUSEPORT` | 16,426 – 16,935 | 79 – 83% | 5 / 4 ms | 401 / 223 ms |

Throughput was the same in both modes (that box was at its limit with Postgres on 8 of its cores); what changes is
that no worker saturates while others idle.

### Memory (4 vCPU VPS)

The server's RSS grew from 230 MB at 20k connections to 1,090 MB at 160k: about 6 KB per open connection, plus
roughly 3 KB of kernel socket memory. A million connections should need around 10 GB.

`test/run.sh`: `passed=42 failed=0`

## Notes

- facil.io writes response headers as `name:value` (no space after the colon). That is valid HTTP, but
  `test/test.sh` reads the content type by splitting on a space, so the `content-type` value is written with a
  leading space to produce `content-type: application/json`.
