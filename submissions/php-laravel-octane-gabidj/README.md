# php-laravel-octane: Laravel 12 + Octane (Swoole) + PDO pgsql

A Laravel app served by Laravel Octane on the Swoole extension. Not benchmarked yet: the numbers will come from the
challenge run. Everything below was tested with `test/run.sh` (42/42) on Ubuntu 24.04 against Postgres 18.

## Stack

| | |
|---|---|
| Language | PHP 8.3.6 (Ubuntu 24.04 `php8.3-cli`, opcache on for the CLI) |
| Framework | Laravel 12.70.2 (`composer.lock` pins every package) |
| Server | Laravel Octane 2.22.0 on Swoole 6.2.4, built from the PECL tarball by `install.sh` (Ubuntu has no `php-swoole`) |
| Postgres driver | `pdo_pgsql` (libpq), through Laravel's `pgsql` connection |
| JWT | hand-written HS256 check (`hash_hmac` + `hash_equals`), on every request |

```bash
sudo bash install.sh   # php, composer, swoole with pgsql (~3 min: it compiles)
bash build.sh          # composer install --no-dev from the lock, route:cache
DATABASE_URL=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

Optional env: `WORKERS` (total PHP workers), `DB_POOL` (Postgres connections per worker), `MAX_REQUESTS`.

## Process and connection model

- **Two Octane servers, one per port.** Octane's Swoole server listens on one port, so `start.sh` runs
  `artisan octane:start` twice (`PORT` and `EXTRA_PORT`, each with its own state file) and splits the workers and
  the reactor threads evenly. The clients spread over the two ports evenly, so the halves carry equal load. If one
  server dies, `start.sh` stops the other and exits (rule 10: no restarts).
- **Workers:** one per CPU (`nproc`, so it follows CPU affinity), `WORKERS` overrides. A worker is not blocked while it
  waits for Postgres, see below, so it only needs a core's worth of CPU.
- **Coroutines, many requests per worker.** `enable_coroutine` is on, so each request runs in its own Swoole
  coroutine, and `SWOOLE_HOOK_PDO_PGSQL` makes `pdo_pgsql` yield to the scheduler while it waits for Postgres
  (Swoole is built with `--enable-swoole-pgsql` for that, `install.sh`). While one request waits for its query, the
  worker runs the others. `App\Support\Db` is a per-worker pool of up to `DB_POOL` (default 8) connections, built by
  Laravel's own `PostgresConnector`; a request borrows one for a single statement. Total Postgres connections =
  workers x pool, at most 480 (rule 5). Only `pdo_pgsql` is hooked, so requests interleave only at a database call.
- **Octane without the per-request app swap.** Octane's default listeners hand each request's cloned copy of the app
  to the shared kernel, router and managers, and flush it when the request ends. With overlapping requests a request
  that resumed after its query found the kernel pointing at another request's flushed container
  (`Target class [events] does not exist`: about 2% of responses were Octane's own 500). `config/octane.php` drops those
  listeners, so the kernel and router keep the base app. That is safe here because nothing in the container is
  per-request: the request travels as an argument, and the error path never uses the container or the logger.
- **Reactor threads** (network I/O, HTTP parsing): one per CPU in total. Swoole's default `dispatch_mode` pins a
  connection to a worker, which is fine now that a worker is never blocked on a query.
- **Idle connections** are never closed by the server (no `heartbeat_*`); `max_conn` is 1,048,576 per server.
- `max_request` is 100,000: a worker is recycled after that many requests, as insurance against leaks.
- **PHP:** tracing JIT (64M buffer) and opcache preloading of the classes a request touches (`preload.php`, about 760
  classes, linked once in the server process before the workers fork).

## What is Laravel and what was cut

Laravel boots once per worker; each request goes through the router, the container and the exception handler.
Cut: the web/api middleware groups and every global middleware (no sessions, cookies, CSRF, `TrimStrings`,
maintenance check), the config cache (it would freeze `DATABASE_URL` at build time), file logging (errors go to
stderr, and the expected 4xx are not reported). Routes are cached by `build.sh`.

- **Auth** is a route middleware (`Authenticate`), so it runs before the controller's id check, as the spec requires.
- **SQL** is the spec's SQL, one statement per request. `Db` keeps each `PDOStatement` (a server-side prepared plan:
  Parse once per connection, then Bind/Execute) cached per pooled connection. Only plans are cached, never data. If a
  connection drops it is discarded and the call is retried once on a new one.
- **Timestamps:** the session time zone is UTC (`config/database.php`), and `created_at` is formatted from
  Postgres's text with string slicing, truncated to ms.
- **JSON:** `json_encode` of arrays in spec order, `Content-Type: application/json`. Swoole gzips the body when the
  client sends `Accept-Encoding: gzip` (level 1).
