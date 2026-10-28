# Crystal + Kemal + crystal-pg

The feed API written the way a Kemal app normally is: routes, `crystal-pg` through `crystal-db`'s pool,
`JSON::Builder`, Kemal's gzip handler. The no-framework version is
[`crystal-epoll-faustinoaq`](../crystal-epoll-faustinoaq).

| | |
|---|---|
| Language | Crystal 1.22.1 (official tarball, sha256-checked by `install.sh`) |
| Framework | Kemal 1.15.0 (Crystal's `HTTP::Server`, one fiber per connection) |
| Driver | crystal-pg 0.31.0 on crystal-db's pool |
| JWT | jwt 1.7.2 (HS256) |
| Gzip | yes: Kemal's `gzip true` (stdlib `Compress::Gzip`) |

`build.sh` targets the fixed x86-64-v3 (AVX2) baseline because native CPU detection
produced SIGILL on some CI runners.

## How it runs

- Crystal's runtime is one thread per process here, so `start.sh` starts **one process per core**
  (`WORKERS`, default `nproc`). Each binds `PORT` and `EXTRA_PORT` with `SO_REUSEPORT`, and the kernel
  spreads the connections over all of them.
- Each process has a pool of `500 / WORKERS` Postgres connections (at most 64), so the total stays
  under rule 5's 512.
- The SQL is the spec's, unchanged, as prepared statements (crystal-pg prepares every parameterized
  query). `created_at` is formatted in UTC with milliseconds; a foreign-key violation (`23503`) on a
  like is a 404.
- `start.sh` raises the soft descriptor limit to the hard one, and stays in the foreground until the
  processes exit.

## Local numbers

Ryzen 5 3600 (WSL2), Postgres 18 on the same machine, the server on 4 cores (`WORKERS=4`), wrk with
`Accept-Encoding: gzip`, 256 connections:

| | this | `examples/rust` (axum + SQLx), same setup |
|---|---:|---:|
| `/feed` | 4,000 req/s | 3,600 req/s |
| mix (46% feed, 46% post, 7% like, 1% create) | 6,000 req/s | 5,000 req/s |

Postgres shares the box in these runs, so they say little about the scored setup.

## Run

```bash
sudo bash install.sh
bash build.sh
DATABASE_URL=postgres://... JWT_SECRET=... HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```
