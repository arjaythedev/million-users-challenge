# php-ffi-latteo: Pure PHP 8.3 on raw Linux epoll via built-in FFI

A zero-dependency implementation of the Million Users Challenge API (`SPEC.md`) in a single PHP file (`index.php`), using only standard Ubuntu 24.04 packages (`php8.3-cli`, `php8.3-pgsql`, `php8.3-opcache`) — no Composer packages, no PECL extensions (no Swoole, Workerman, RoadRunner, or FrankenPHP), and no web server in front.

Instead of PHP's blocking stream wrapper, each worker calls Linux's `epoll_create1`, `epoll_ctl`, `epoll_wait`, `accept4`, `read`, `write`, and `close` directly in `libc.so.6` through PHP's built-in `FFI` extension.

## Stack

| Piece | Version |
|---|---|
| Language / Runtime | PHP 8.3.6 (`php8.3-cli` from Ubuntu 24.04 apt), OPcache + Tracing JIT (`opcache.jit=1255`, 64 MB buffer) |
| Event loop & HTTP | Raw Linux `epoll` (`libc.so.6` via built-in `FFI`), custom zero-dependency HTTP/1.1 Keep-Alive server |
| Process model | Pre-forked worker pool via built-in `pcntl_fork` (`SO_REUSEPORT` on `PORT` and `EXTRA_PORT`) |
| Postgres driver | `ext-pgsql` (`php8.3-pgsql` / `libpq`), server-side prepared statements (`pg_prepare` / `pg_execute`) |
| JWT | Hand-written HS256 verification (`hash_hmac` + `hash_equals` + `exp` check) on every authenticated request |
| gzip | Built-in `gzencode` (level 1) when `Accept-Encoding: gzip` is present and body > 256 bytes |

```bash
sudo bash install.sh   # apt: php8.3-cli php8.3-pgsql php8.3-opcache
bash build.sh          # syntax check (php -l)
DATABASE_URL=postgres://postgres:postgres@localhost:5432/feed \
  JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

## Design & Optimizations

- **Raw `epoll` through built-in `FFI` (`libc.so.6`):**
  - PHP's built-in `stream_select()` is limited by `FD_SETSIZE` (1024 descriptors) and $O(N)$ scanning. By binding directly to `epoll_create1`, `epoll_ctl`, `epoll_wait`, and `accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC)` via `FFI::cdef`, each worker handles tens of thousands of open HTTP/1.1 Keep-Alive sockets with $O(1)$ readiness notifications.
  - Idle connections hold **zero PHP objects or buffers** — only an OS file descriptor registered in the kernel's epoll interest list (`EPOLLIN | EPOLLRDHUP`). A per-worker pre-allocated `char[16384]` FFI buffer reads incoming frames in place; an associative entry in `$partial[$fd]` is allocated only on the rare occasion a request header or body splits across TCP segments, and is `unset()` immediately once complete.
- **Multi-process `SO_REUSEPORT` workers (`pcntl_fork`):**
  - `start.sh` forks `WORKERS` processes (default `32`, configurable via `WORKERS`).
  - Every worker binds its own non-blocking listening sockets on both `PORT` and `EXTRA_PORT` with `SO_REUSEADDR`, `SO_REUSEPORT`, `TCP_NODELAY`, and a `65535` listen backlog. The Linux kernel distributes incoming connections across workers without an accept lock or cross-process IPC.
- **One persistent `libpq` connection and prepared plan set per worker:**
  - Each worker opens a single persistent PostgreSQL connection (`pg_connect` with `options='--timezone=UTC'`) and prepares the four `SPEC.md` SQL statements once (`pg_prepare`: `feed`, `get_post`, `insert_post`, `insert_like`).
  - At the default 32 workers, total PostgreSQL connections are **32** (well below the 512 connection limit in Rule 5).
  - For `POST /posts/:id/like`, asynchronous dispatch (`pg_send_execute` + `pg_get_result`) is used so PostgreSQL's `SQLSTATE 23503` (`foreign_key_violation`) is inspected directly from the result diagnostics (`pg_result_error_field(..., PGSQL_DIAG_SQLSTATE)`) and mapped to `404 {"error":"post not found"}` in a single database round-trip.
  - Automatic connection recovery (`pg_connection_reset` / reconnect + re-prepare) handles transient database resets cleanly.
- **Fast string & JSON paths:**
  - UTC timestamps from PostgreSQL (`options='--timezone=UTC'`) are formatted to ISO-8601 millisecond precision (`YYYY-MM-DDTHH:MM:SS.mmmZ`) with direct byte-offset slicing instead of `DateTimeImmutable` parsing.
  - UTF-8 code point length validation on `POST /posts` counts non-continuation bytes (`preg_replace('/[\x80-\xBF]/', '', $trimmed)`) after Unicode whitespace trimming (`/^\s+|\s+$/u`).
  - OPcache CLI (`opcache.enable_cli=1`) and PHP 8.3 Tracing JIT (`opcache.jit=1255`, `opcache.jit_buffer_size=64M`) compile hot HTTP parsing and request handling loops to native machine code.

## Rules Compliance

- **Rule 2 & 3:** Uses standard `SPEC.md` SQL queries (one query per request), prepared once per connection. No SQL-side JSON construction, extra queries, or server-wide config changes.
- **Rule 5:** Uses 1 connection per worker (`32` total by default, max `<= 512`).
- **Rule 6:** Passes 42/42 checks in `test/test.sh`.
- **Rule 7:** Zero caching of query results, responses, or verified JWTs across requests.
- **Rule 8:** Every write (`POST /posts`, `POST /posts/:id/like`) waits for PostgreSQL commit before sending the HTTP response; `synchronous_commit` is untouched.
- **Rule 9:** Every authenticated request verifies the Bearer token's `HS256` header, HMAC-SHA256 signature (`hash_equals`), `exp` timestamp, and `sub`/`username` payload claims.
