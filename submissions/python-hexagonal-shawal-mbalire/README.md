# python-hexagonal-shawal-mbalire

A Python 3.12 server built as a **hexagon (ports and adapters)**. You can swap the HTTP server, the
web framework, the Postgres driver and the JSON encoder with one environment variable each, and
the core doesn't change. The domain layer also carries the reasoning: the queueing model that
sizes the processes and connections, the timestamp contract, and the operation semantics that
the adapters read.

The default stack (what the benchmark runs) is **raw sockets on uvloop + libpq in pipeline mode
(a C extension) + a C JSON encoder**.

## Stack

| | |
|---|---|
| Language | Python 3.12 (Ubuntu 24.04 `python3`) + two small C extensions built on the box |
| Event loop | uvloop 0.23.0, one per worker process, eager task factory |
| HTTP (default) | none: HTTP/1.1 parsed by hand on uvloop transports (`feed/adapters/inbound/raw.py`) |
| Postgres (default) | libpq 18 (apt.postgresql.org) in **pipeline mode**, through `_pq.c` |
| JSON (default) | `_cjson.c`: one C call writes a whole response |
| JWT | stdlib `hmac.digest` (HS256 only), no library |
| Also selectable | uvicorn 0.54.0, granian 2.8.7, FastAPI 0.143.3 (Starlette 1.8.0, Pydantic 2.14.1), asyncpg 0.32.0, orjson 3.13.0 |

`requirements.txt` pins every package, transitive ones included. `install.sh` (root) installs
Python, a C compiler and libpq from the PostgreSQL project's apt repository. `build.sh` (user)
creates the venv, compiles both extensions, and runs the 36 unit tests.

## Architecture

```
feed/
  domain/          pure Python, no I/O, imports nothing outside itself
    model.py         Principal, PostView (and why they are tuples)
    rules.py         post id and body invariants
    errors.py        the failure vocabulary
    timefmt.py       UTC, millisecond, truncated (and why truncated)
    semantics.py     @query / @command(idempotent=...): what an operation is
    capacity.py      Little's law, workers per core, read/write lanes, the 512-connection cap
  application/     the use cases and the ports they need
    service.py       FeedService: one method per endpoint, declared with @route + @query/@command
    routes.py        @route and the Router every HTTP adapter shares
    ports.py         PostRepository, Authenticator, JsonCodec (typing.Protocol)
    contract.py      Reply(status, bytes) and the one error table
  adapters/
    inbound/         raw.py, uvicorn_server.py, granian_server.py   (servers)
                     asgi_bare.py, fastapi_app.py, rsgi_bare.py     (apps)
    outbound/        postgres/{libpq.py,_pq.c,asyncpg_repo.py,pgjson.py}
                     json/{cjson.py,_cjson.c,handwritten.py,orjson_codec.py,pydantic_codec.py,pgjson.py}
                     jwt_hs256.py
    profiling.py     timing decorators for any port, plus cProfile
  runtime/         prefork supervisor, SO_REUSEPORT sockets, configuration
  bootstrap.py     composition root: the registries, and the only module that knows every adapter
```

The dependency rule (the domain imports only itself; the application imports only the domain)
is **enforced by `tests/test_architecture.py`**, which parses every import. If that test fails,
the build fails.

### Decorators: declared in the core, adapted at the edge

The core uses two kinds of decorator. Both only state facts: they return the same function, so
they cost nothing at run time. The adapters turn those facts into their own mechanisms:

| Declared in the core | Adapted by |
|---|---|
| `@query` / `@command` on `PostRepository` | libpq: queries go to **read lanes**, commands to **write lanes**. asyncpg: the read pool or the write pool. Neither adapter keeps its own list of which methods write |
| `@command(idempotent=True)` on `like` | profiling labels; it's also the hook a retry policy would need (only queries and idempotent commands are safe to retry) |
| `@route("POST", "/posts/{id}/like", needs=("authorization","id"))` | FastAPI: generated `app.add_api_route(...)` calls (its own decorator). ASGI, RSGI and raw sockets: `Router.match` |
| any port | `FEED_PROFILE=latency` wraps it in a timing proxy that keeps all of the declarations above |

## Adapters

| Variable | Values (default first) | Port |
|---|---|---|
| `FEED_SERVER` | `raw`, `uvicorn`, `granian` | who accepts connections |
| `FEED_APP` | raw: `raw` · uvicorn: `bare`, `fastapi` · granian: `rsgi`, `bare`, `fastapi` | who turns HTTP into calls |
| `FEED_DB` | `libpq`, `asyncpg`, `pgjson`* | `PostRepository` |
| `FEED_JSON` | `cjson`, `handwritten`, `orjson`, `pydantic`, `pgjson`* | `JsonCodec` |
| `FEED_PROFILE` | off, `latency`, `cprofile`, `latency,cprofile` | decorators |
| `FEED_WORKERS`, `FEED_READ_LANES`, `FEED_WRITE_LANES` | from `domain/capacity.py` | topology overrides |

\* `pgjson` builds the post JSON in SQL (`row_to_json`). **That breaks rule 3**, so the server
refuses to start with it unless `FEED_ALLOW_NONCOMPLIANT=1`. It's a controlled experiment: the
same lanes and the same pipelining, with only the place where JSON gets built changed.

Adapters are imported lazily, so the default worker never loads FastAPI, Pydantic, granian or asyncpg.

## Optimizations, and why

- **One event loop per core, each with its own `SO_REUSEPORT` socket on both ports.** A CPython
  process runs Python on one core at a time, so more loops than cores only adds time-slicing.
  Per-worker sockets let the kernel hash connections evenly; with a shared accept queue the
  hottest loop caps the whole box. The core count comes from the affinity mask and the cgroup
  CPU quota, not `os.cpu_count()`, so the 8- and 4-core runs size themselves correctly.
- **libpq pipeline mode, one flush per loop iteration.** Many queries are in flight per
  connection, and Postgres answers them in order, so a FIFO of futures *is* the correlation. Each
  query gets its own sync point (`PQsendPipelineSync`, libpq 17+, which doesn't flush), so a
  failed statement aborts only itself. Everything queued during one loop iteration leaves in one
  `write(2)`: Nagle's batching without Nagle's delay.
- **Separate write lanes.** A pipelined connection is FIFO, so an INSERT waiting on its WAL flush
  (`synchronous_commit` stays on) would block every read queued behind it. Writes get their own
  connection per worker, and Postgres still group-commits across all of them.
- **Lane count from Little's law** (`domain/capacity.py`): about 48 read backends in total,
  whatever the core count, and 64 connections at 16 cores (the cap is 512).
- **Rows decoded straight into domain tuples in C.** `timestamptz` arrives as text in a UTC/ISO
  session (session-level `SET`, allowed by rule 2) and is truncated to milliseconds in C. A test
  checks it against the domain's Python reference.
- **A C JSON encoder.** One C call writes the whole feed into one buffer. All four codecs are
  tested byte-for-byte equal on hostile input (quotes, backslashes, control characters, emoji).
- **Hand-written HTTP/1.1.** The three headers the contract reads are found with `bytes.find`
  on a lowercased copy of the head. Connections are `__slots__` Protocol objects, and there's
  no per-connection idle timer (the contract wants ≥ 75 s; never closing idle connections
  satisfies that). HTTP pipelining is answered in order.
- **gzip level 1, feed only (≥ 1 KB).** Single posts and errors are too small to be worth the CPU.
- **JWT: one `hmac.digest` + `compare_digest`**, with the signature checked before any JSON is
  parsed. `exp` is required. Nothing is remembered between requests (rule 9).
- **`gc.freeze()` after startup + a higher gen-0 threshold.** Long-lived objects leave the
  collector's generations, so collections only scan per-request garbage.

## Why this default: measured, not guessed

The 16-core score is capped at 1,000,000, so the contest is holding a million users with *as
little compute as possible*: the 8- and 4-core scores. A million users is ~41,700 req/s, i.e.
~10,400 req/s per core at 4 cores. So the default is whatever spends the least CPU per request
and the least memory per connection.

Each combination ran as **one worker pinned to one core**, driven by the challenge's own `mload`
(`-gzip`, the real request mix, 6,000 users, `-think-scale 0.1`, ~5,800 req/s offered), on a
fresh copy of the seed each time. The metric is the worker's CPU time per request
(`/proc/<pid>/stat`), two rounds each. The numbers come from a development machine, so they
rank the adapters rather than predict the box:

| Stack | CPU µs / request | ≈ req/s per core | p99 |
|---|---:|---:|---:|
| **raw + libpq + cjson** (default) | **91–94** | **~10,600–10,900** | 413–686 ms |
| raw + libpq + handwritten | 92–95 | ~10,500–10,800 | 374–470 ms |
| raw + libpq + pydantic | 90–95 | ~10,500–11,100 | 447–474 ms |
| granian RSGI + libpq + cjson | 105–106 | ~9,400–9,500 | 352–429 ms |
| raw + asyncpg + cjson | 128–130 | ~7,700–7,800 | 1.16–1.17 s ✗ |
| uvicorn + FastAPI + asyncpg + Pydantic | 294–315 | ~3,200–3,400 | 12–13 s ✗ |

- The HTTP layer and the driver dominate. libpq pipelining saves ~28% of the CPU per request
  over asyncpg, and the classic FastAPI stack costs ~3.3× more per request.
- The JSON codec does not matter at this size: the three codecs differ by less than the
  run-to-run noise (one feed encode is ~30 µs of a ~90 µs request). `cjson` stays the default
  because it does the least Python-level work.

Memory per idle keep-alive connection (12,000 connections into one worker, RSS delta, kernel
socket memory not included): **raw ≈ 1.3 KB**, granian ≈ 8.9 KB. So a million connections cost
~1.3 GB of Python heap on the default stack, and the 3,000,000-user search on 8 and 4 cores stays
well inside 32 GB.

## Rules compliance

These hold for the default configuration:

- The spec's SQL, verbatim, one statement per request (`adapters/outbound/postgres/sql.py`).
- Prepared statements and pipelining only.
- No caching of any kind.
- Writes are committed before the response.
- Every token's signature and `exp` are checked on every request.
- At most 512 connections, enforced at startup.
- The schema is never touched.
- `pgjson` is the one exception, and it is gated as described above.

## Profiling

```bash
FEED_PROFILE=latency bash start.sh       # per-port log2 histograms: p50/p95/p99/max per operation
kill -USR1 <master pid>                  # every worker prints its report to stderr
ls profiles/                             # worker-N.latency.txt (and worker-N.prof with cprofile), every 10 s
python -m pstats profiles/worker-0.prof  # with FEED_PROFILE=cprofile
py-spy top --pid <worker pid>            # sampling, zero instrumentation (not a dependency)
```

The use-case timings include the database and codec calls made inside them; the difference is
the core's own work. Profiling is off by default and then costs nothing, because nothing is wrapped.

## Verification

- `build.sh` runs 36 unit tests: domain rules, JWT edge cases, the use cases against in-memory
  fakes, codec byte-equivalence, routing, the decorators, profiling transparency, and the
  dependency rule.
- `test/run.sh` passes **42/42 with every combination**:
  - the default;
  - each server/app pairing (raw; uvicorn + bare/FastAPI; granian + RSGI/bare/FastAPI);
  - each `FEED_DB` × `FEED_JSON` pairing (libpq/asyncpg × cjson/handwritten/orjson/pydantic);
  - `pgjson`;
  - with profiling on.
- Idle keep-alive: on raw, granian and uvicorn, connections idle for 80 s are still open and
  served afterwards.
- Not yet load-tested on the challenge hardware. Throughput numbers will come from the benchmark run.

```bash
sudo bash install.sh && bash build.sh
DATABASE_URL=postgres://user:pass@host:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```
