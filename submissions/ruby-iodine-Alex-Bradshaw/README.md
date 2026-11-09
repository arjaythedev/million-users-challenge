# ruby-iodine-Alex-Bradshaw: Ruby 4.0 (YJIT) + Iodine + pg

A Ruby entry. No Rails, no Rack framework: Iodine's C reactor holds the sockets and a thin NeoRack handler does the
per-request work.

## Stack

| | |
|---|---|
| Language | Ruby 4.0.9, built from the official tarball with YJIT (`install.sh`) |
| Server | [Iodine](https://github.com/boazsegev/iodine) 0.8.3 (facil.io C core, epoll) |
| API | NeoRack (`on_http(e)`), not Rack: no env Hash is built per request |
| Postgres driver | [pg](https://github.com/ged/ruby-pg) 1.6.3, compiled from source against the system libpq |
| JSON | stdlib `json` for parsing request bodies and escaping strings that need it; responses are built by hand |
| JWT | hand-rolled HS256 check with `OpenSSL::HMAC` + `OpenSSL.fixed_length_secure_compare` (no `jwt` gem) |
| gzip | Iodine's dynamic deflate, in C |

```bash
sudo bash install.sh   # Ruby 4.0.9 + YJIT into /opt/ruby-4.0.9, plus libpq-dev etc.
bash build.sh          # pinned gems into ./vendor/bundle
DATABASE_URL=postgres://user:pass@host:5432/feed JWT_SECRET=million-users-challenge HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

Optional env: `WORKERS` (default: one per core in the CPU affinity mask), `THREADS` (Ruby threads per worker,
default 6), `RUBY_PREFIX` (default `/opt/ruby-4.0.9`), `MALLOC_ARENA_MAX` (default 2).

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
- No Nginx: Iodine listens on `PORT` and `EXTRA_PORT` directly from the same workers.
- Keep-alive timeout is 120 s (SPEC: at least 75 s).

## Ideas not done yet

- **Measure memory per idle connection** at 100k+ connections; that decides whether a million fits.
- Tune `THREADS` (and `WORKERS` above the core count) with `bench/mload`.
- Check how evenly Iodine spreads connections over workers (it accepts on a shared listener) and whether one
  listener per worker would balance better.
- Batch writes (likes, posts) from several threads into one transaction (allowed by the rules).
- GC tuning (`RUBY_GC_HEAP_*`), and a cheaper non-exception path for auth/validation errors.

## Results (local, rough)

On a 2-vCPU sandbox with Postgres 16 and `mload` sharing the same two cores (so the numbers only show the shape):

- `test/run.sh`: `passed=42 failed=0`
- `mload -users 9000 -gzip -think-scale 2.5`: pass, p95 2.7 ms, 0 errors
- Memory: each worker went from ~38 MB with 1,000 connections to ~60 MB with 4,500, i.e. roughly 6 KB of user-space
  memory per open connection (before kernel socket memory). Extrapolated to a million connections that is ~6 GB
  plus kernel buffers, which should fit in 32 GB, but it needs measuring at 100k+ on a real box.
- Saturated (`-users 3000 -think-scale 0.05`, 2 workers x 6 threads): ~3,500 req/s.

## Notes

- facil.io writes response headers as `name:value` (no space after the colon). That is valid HTTP, but
  `test/test.sh` reads the content type by splitting on a space, so the `content-type` value is written with a
  leading space to produce `content-type: application/json`.
