# forth-gforth (pipelined): epoll server in Forth, pipelined libpq to Postgres

The feed API from `SPEC.md` in **gforth**, no web framework and no HTTP library: a
hand-written **epoll** event loop, one process per core, talking to PostgreSQL through
**libpq**. HTTP/1.1, JSON and JWT HS256 are hand-rolled Forth.

This is a sibling of `forth-gforth-FantasticForth` (same author, same from-scratch engine).
The one difference is the **data layer**: instead of one blocking `PQexecPrepared` per
worker, this entry drives a **pool of libpq connections in pipeline mode**, non-blocking,
on the same epoll loop. SPEC.md rule 3 explicitly allows this ("you may prepare them,
pipeline them and batch the writes"); the SQL is the SPEC SQL verbatim.

## Stack

| Piece | Version / detail |
|---|---|
| Language / runtime | gforth 0.7.3 (Ubuntu 24.04 archive build) |
| HTTP | hand-written HTTP/1.1 parser + response builder (`http.fs`), no library |
| JSON | hand-written compact emitter + strict request parser (`json.fs`) |
| JWT | hand-written HS256 verify (`jwt.fs`) over libcrypto `HMAC`/`EVP_sha256` via FFI |
| Postgres driver | libpq (libpq-dev 16.x) via gforth's `c-library` FFI, **pipeline mode** |
| Process model | one worker process per core (`SO_REUSEPORT`), shared-nothing |

## Design

Same front end as the sibling entry — thread/process-per-core, `SO_REUSEPORT` listeners on
both ports, a fixed slot pool of connections with lazily-faulted per-connection buffers so a
worker can hold a very large number of idle keep-alive connections cheaply.

**What changed: the data layer (`pg-pipe.fs`).**

- Each worker owns a small **pool** of libpq connections — `DB_POOL` total (default 6),
  split into read connections and `DB_WRITE` write connections (default ~1/3, min 1). All
  are put in `PQenterPipelineMode` and `PQsetnonblocking(1)`.
- Every connection's `PQsocket` is registered in the **same epoll set** as the client
  sockets. A request is parsed, its prepared statement is sent with `PQsendQueryPrepared`
  (no blocking), and the client slot is parked (`ST-DBWAIT`) with a note of which formatter
  to run. When the connection's socket is readable, `PQconsumeInput` + `PQgetResult` collect
  finished results, matched back to parked slots through a per-connection FIFO ring (results
  return in send order), and the response is formatted and written.
- **Reads batch**, **writes don't**: each event-loop pass sends one `PQpipelineSync` per read
  connection with queued statements (one round trip per connection per pass). Writes get
  their own Sync at submit time, so each is its own transaction — a like on a missing post
  fails alone (SQLSTATE 23503 -> 404) without rolling back a read batch, and the 201/200
  response is held until that Sync's result arrives (post-commit, rule 8).
- A dropped connection fails its in-flight requests with 500 and is reconnected.

The whole point: the blocking design does **one** Postgres round trip at a time per worker,
so under load the event loop sits idle waiting on the DB while the box has CPU to spare.
Pipelining keeps many queries in flight per worker, converting that idle time into
throughput.

## Measured effect

Two-box benchmark (app + Postgres + load, 16-core `c7i.4xlarge` each, one AZ), `GET /feed`,
`WORKERS=16`, aggregate throughput:

| | throughput |
|---|---|
| blocking (sibling entry) | ~28,000 req/s |
| this entry (`DB_POOL=4`) | ~50,000 req/s (**~1.8x**) |

Under the pipelined server the **app host is ~43% CPU while Postgres is ~96%** — the
per-worker serialization is gone and the single DB box is now the limit (as expected for an
app-layer change). Small pools win: `DB_POOL` of 4–8 per 16-worker host is the sweet spot;
larger pools just pile concurrent load onto Postgres and reduce throughput.

## Config (env, per SPEC runtime contract)

`DATABASE_URL`, `JWT_SECRET`, `HOST`, `PORT`, `EXTRA_PORT` as specified. Plus two optional
tuning knobs with safe defaults:

| Env | Default | Meaning |
|---|---|---|
| `DB_POOL` | 6 | total libpq connections per worker |
| `DB_WRITE` | ~`DB_POOL/3`, min 1 | how many of those are write connections |
| `WORKERS` | `nproc` | worker processes (one per core) |

## Files

`install.sh` (toolchain on Ubuntu 24.04), `build.sh` (forces the FFI wrapper to compile +
`compile-check.fs`), `start.sh` (foreground, `exec gforth -m 512M server.fs`), then the Forth
layers: `ffi.fs` `pg.fs` `pg-pipe.fs` `jwt.fs` `json.fs` `http.fs` `epoll.fs` `http-async.fs`.

Note: `start.sh` passes `gforth -m 512M` (a larger Forth dictionary) because the extra
per-slot state arrays for the async pipeline exceed gforth's default dictionary size.
