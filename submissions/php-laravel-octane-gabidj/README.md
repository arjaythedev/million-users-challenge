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
sudo bash install.sh   # php, composer, swoole (~3 min: it compiles)
bash build.sh          # composer install --no-dev from the lock, route:cache
DATABASE_URL=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

Optional env: `WORKERS` (total PHP workers), `MAX_REQUESTS`.

## Process and connection model

- **Two Octane servers, one per port.** Octane's Swoole server listens on one port, so `start.sh` runs
  `artisan octane:start` twice (`PORT` and `EXTRA_PORT`, each with its own state file) and splits the workers and
  the reactor threads evenly. The clients spread over the two ports evenly, so the halves carry equal load. If one
  server dies, `start.sh` stops the other and exits (rule 10: no restarts).
- **Workers:** `6 x CPUs` in total (`nproc`, so it follows CPU affinity), capped at 480. A worker runs one request at a
  time and blocks on Postgres, so it needs more workers than cores to keep the cores busy. **Each worker holds one
  Postgres connection**, opened on its first query, so the connection count is the worker count (96 on 16 cores,
  never above 480, under rule 5's 512).
- **Reactor threads** (network I/O, HTTP parsing): one per CPU in total.
- **`dispatch_mode = 3`:** the next request goes to an idle worker. Swoole's default pins a connection to one worker
  (`fd % workers`); with blocking workers and 500k keep-alive connections per port, a busy worker would stall every
  connection pinned to it while others sit idle.
- **Idle connections** are never closed by the server (no `heartbeat_*`); `max_conn` is 1,048,576 per server.
- `max_request` is 100,000: a worker is recycled after that many requests, as insurance against leaks.

## What is Laravel and what was cut

Laravel boots once per worker; each request goes through the router, the container and the exception handler.
Cut: the web/api middleware groups and every global middleware (no sessions, cookies, CSRF, `TrimStrings`,
maintenance check), the config cache (it would freeze `DATABASE_URL` at build time), file logging (errors go to
stderr, and the expected 4xx are not reported). Routes are cached by `build.sh`.

- **Auth** is a route middleware (`Authenticate`), so it runs before the controller's id check, as the spec requires.
- **SQL** is the spec's SQL, one statement per request. `Db` keeps each `PDOStatement` (a server-side prepared plan:
  Parse once per connection, then Bind/Execute) cached per worker. Only plans are cached, never data. If the
  connection drops, the connection and the plans are rebuilt and the call is retried once.
- **Timestamps:** the session time zone is UTC (`config/database.php`), and `created_at` is formatted from
  Postgres's text with string slicing, truncated to ms.
- **JSON:** `json_encode` of arrays in spec order, `Content-Type: application/json`. Swoole gzips the body when the
  client sends `Accept-Encoding: gzip` (level 1).
