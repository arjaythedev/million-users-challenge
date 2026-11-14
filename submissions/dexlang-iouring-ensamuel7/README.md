# DexLang (generated C, io_uring)

| | |
|---|---|
| **Language** | [DexLang](https://github.com/ensamuel7/dex) `v0.9.34`: statically typed, compiles to C, which `cc` builds into a native binary |
| **Framework** | None. DexLang's own `http`, `db` and `json` standard library modules |
| **Server** | The `http` module's io_uring backend (`http.uring()`), one event loop per process, `http.fork(n)` processes sharing ports 80 and 81 through `SO_REUSEPORT` |
| **Postgres driver** | libpq (Ubuntu's `libpq-dev`) in pipeline mode, through the `db` module: the five statements from SPEC.md, prepared, one connection per process |
| **Build toolchain** | Go 1.27.2 to build the compiler, then `cc` (gcc 13) on the C it emits |

What runs is one native binary: no interpreter, no garbage collector, no
framework. The program is about 420 lines in `server.dx` and `queries.dx`, and
everything below it is the language's standard library.

## How it serves

**Processes, not threads.** `start.sh` starts four workers per core it is given
(`nproc`, so 64 on 16 cores, 16 on 4). Before opening anything the server calls
`http.fork(n)`; each copy opens its own Postgres connection, prepares the five
statements, and listens on both ports with `SO_REUSEPORT` (`http.extraPort(81)`),
so the kernel spreads connections across processes. A worker owns its sockets,
its connection and its statements: no lock, no pool and no handoff on a
request's path. 64 connections is well under the 512 allowed.

**One round trip per pass, not per request.** A handler checks its input, binds
its one statement and sends it with `db.send`, which doesn't wait, then takes a
ticket with `http.hold()`. After the loop has run every request that arrived in
that pass, the `http.afterBatch` hook reads the results. The first read sends
everything the pass queued in one write, and the rows come back in one round
trip, in order. The hook answers each ticket with `http.reply()`. The worker
waits on the network once per pass rather than once per request.

Every statement carries its own Sync, so each is still its own implicit
transaction: a like on a missing post fails alone. A result is read only once
the ReadyForQuery of its own Sync has arrived, which is when its transaction has
committed (`dex_db_pipe_collect` in the runtime's `stdlib/cruntime/db.c`). A
commit that fails after CommandComplete, or a connection lost before the Sync,
reads as a failed statement and gets a 500, never a 201.

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
seed on a separate box (0.18 ms away) and `mload -gzip -think-scale 2.5` on two
others, 60 s ramp and 2 minutes held, 80,000 users. With one worker, so that one
process gets about as many requests per second as a worker on the 4-core
benchmark box:

| version | req/s | p50 | p95 | p99 | errors | server CPU |
|---|--:|--:|--:|--:|--:|--:|
| one blocking statement per request | 2,426 | 888 ms | 3,911 ms | 56,847 ms | 2.7% (timeouts) | 0.12 cores |
| `db.send`, one round trip per pass | 3,060 | 2.2 ms | 14.4 ms | 33.6 ms | 0% | 0.12 cores |

The blocking worker spends about 0.5 ms waiting for each request's round trip
and query, so at 3,000 requests a second it falls behind and the queue grows
without bound. The pipelined worker waits once per pass and keeps up.

With four workers both versions pass at 80,000 users (p99 4.6 ms and 5.0 ms):
the two load boxes can't open many more connections, so this rig can't reach
the rates where the benchmark failed.

**Gzip.** Every request accepts gzip, and `http.gzip(1024)` compresses any
success body of 1 KB or more with libdeflate at its fastest level, adding
`Content-Encoding: gzip` and `Vary: Accept-Encoding`. That is the feed: the
seeded one goes from 5,303 bytes to 1,835. A single post is too small to gain
and goes out as it is. Nothing compressed is kept; each response is compressed
when it's sent.

## What the language gained from this challenge

- `db.send(stmt)` (v0.9.32): run a bound Postgres statement without waiting.
  Statements sent before one of their results is read share one round trip,
  and each is still its own transaction. v0.9.34 reads a result only after its
  own Sync, and holds up to 65,536 at once (1,024 before, which turned the rest
  of a very large pass into 500s).
- `http.gzip(minBytes)` (v0.9.32): gzip large success bodies for clients that
  accept it, through libdeflate when it is installed at build time.
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
dex.mod      module file: dex 0.9.34
install.sh   root, once: build-essential, pkg-config, libpq-dev, libssl-dev,
             libcurl, libsqlite3-dev, libdeflate-dev, and Go 1.27.2 (sha256-checked)
build.sh     builds the dex compiler from the pinned v0.9.34 tag through the Go
             module proxy (checksum-verified), then compiles server.dx
start.sh     raises the open-file limit and runs four workers per core
```
