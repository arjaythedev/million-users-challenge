# DexLang (generated C, io_uring)

| | |
|---|---|
| **Language** | [DexLang](https://github.com/ensamuel7/dex) `v0.9.27`: statically typed, compiles to C, which `cc` builds into a native binary |
| **Framework** | None. DexLang's own `http`, `db` and `json` standard library modules |
| **Server** | The `http` module's io_uring backend (`http.uring()`), one event loop per process, `http.fork(n)` processes sharing ports 80 and 81 through `SO_REUSEPORT` |
| **Postgres driver** | libpq (Ubuntu's `libpq-dev`), through the `db` module: the five statements from SPEC.md, prepared, one connection per process |
| **Build toolchain** | Go 1.27.2 to build the compiler, then `cc` (gcc 13) on the C it emits |

What runs is one native binary: no interpreter, no garbage collector, no
framework. The program is about 380 lines in `server.dx` and `queries.dx`, and
everything below it is the language's standard library.

## How it serves

**Processes, not threads.** `start.sh` starts six workers per core it is given
(`nproc`, so 96 on 16 cores, 24 on 4). Before opening anything the server calls
`http.fork(n)`; each copy opens its own Postgres connection, prepares the five
statements, and listens on both ports with `SO_REUSEPORT` (`http.extraPort(81)`),
so the kernel spreads connections across processes. A worker owns its sockets,
its connection and its statements: no lock, no pool and no handoff on a
request's path. 96 connections is well under the 512 allowed.

**A worker waits on Postgres in its own time.** A handler runs its one
statement and blocks until the database box answers. With six workers per core
there are always others ready to run while one waits on the network.

**io_uring, sized for a million connections.** Accepts, receives and sends are
queued on a ring and submitted together. Accepted sockets go into the ring's
table of direct descriptors, receives use a kernel buffer ring, and an idle
keep-alive connection holds no read buffer of its own between requests. The
connection budget (3.2 million) is split across the workers, so each opens only
the ring it needs. The idle timeout is 90 s.

## The database

- **The SQL is SPEC.md's**, one statement per request, prepared once per
  connection.
- **Likes use the command tag.** The spec's `INSERT … ON CONFLICT DO NOTHING`
  is run as written; `db.affected()` reads how many rows it inserted (1 → 201,
  0 → 200 `already_liked`), and a foreign-key violation (SQLSTATE 23503, read
  with `db.errorCode()`) is the 404 for a post that doesn't exist.
- **Timestamps in a fixed form.** The connection sets `TimeZone=UTC` and
  `DateStyle=ISO`, so `created_at` comes back as text that's cut to
  milliseconds (truncated, not rounded) without parsing.
- **Plain TCP to the database.** The connection adds `sslmode=disable`, so it
  never negotiates TLS even against a Postgres that offers it (the benchmark's
  database box has `ssl = off` already).

Nothing is cached. Every request reads Postgres.

## Auth

HS256 is verified on every request by recomputing the MAC in base64url and
comparing it in constant time. `alg` must be HS256, `exp` is required, and the
`sub` and `username` claims are checked as SPEC.md lists.

## Measured

On a 2-vCPU Synteq VPS (Xeon Platinum 8260, 4 GB), with Postgres 18 and the
seed on a separate box and `mload -gzip -think-scale 2.5` on two others, 60 s
ramp and 2 minutes held:

| users | req/s | p95 | p99 | errors | server CPU |
|--:|--:|--:|--:|--:|--:|
| 20,000 | 764 | 2.4 ms | 3.1 ms | 0% | 0.11 cores |
| 60,000 | 2,293 | 1.9 ms | 28.7 ms | 0% | 0.22 cores |

That is about 90 µs of CPU per request at 60,000 users, with 2.6 GB of the box's
memory still free while holding the 60,000 connections. A higher run reached
108,000 open connections before the 1-vCPU load box ran out of CPU. Linear
extrapolation from the CPU per request puts 4 cores near 900,000 users. That is
an extrapolation, not a measurement.

Responses are not gzipped.

## What the language gained from this challenge

- `http.fork(n)`: split a program into `n` processes sharing its listening
  ports, so each opens its own database connection before `listen`.
- `http.extraPort(p)`: serve the same routes on another port too, on both the
  epoll and io_uring backends.
- `db.affected`, `db.errorCode`, `db.bindNull` and `db.isNull` for prepared
  statements, on Postgres and SQLite.
- `json.encode` writes optional fields as `null` or their value. It used to
  write a raw pointer.

## Files

```
server.dx    routes, handlers, auth, JSON
queries.dx   the five SQL statements from SPEC.md
dex.mod      module file: dex 0.9.27
install.sh   root, once: build-essential, pkg-config, libpq-dev, libssl-dev,
             libcurl, libsqlite3-dev, and Go 1.27.2 (sha256-checked)
build.sh     builds the dex compiler from the pinned v0.9.27 tag through the Go
             module proxy (checksum-verified), then compiles server.dx
start.sh     raises the open-file limit and runs six workers per core
```
