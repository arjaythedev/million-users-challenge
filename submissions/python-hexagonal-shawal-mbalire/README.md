# python-hexagonal-shawal-mbalire

A Python 3.12 server built as a **hexagon (ports and adapters)**. You can swap the HTTP server, the
web framework, the Postgres driver and the JSON encoder with one environment variable each, and
the core doesn't change. The domain layer also carries the reasoning: the queueing model that
sizes the processes and connections, the timestamp contract, and the operation semantics that
the adapters read.

The default stack (what the benchmark runs) is **a C event loop that serves the two hot reads itself,
with the Python use cases behind it for everything else**, on libpq in pipeline mode. Be clear about
what that means: **94% of requests never enter Python.** Python composes the process, supervises the
workers, and runs writes, JWT checks, `/health` and every error. See
[The C read path](#the-c-read-path-feed_serverfast-the-default). `FEED_SERVER=raw` is the same server
with every request in Python.

## Stack

| | |
|---|---|
| Language | Python 3.12 (Ubuntu 24.04 `python3`) + small C extensions built on the box; the hot reads run entirely in C |
| Event loop (default) | own epoll loop in C, one per worker process (`_fastloop.c`, `inbound/fast.py`) |
| HTTP (default) | none: HTTP/1.1 parsed in place in C. `FEED_SERVER=raw`: parsed in Python on uvloop 0.23.0 transports |
| Postgres (default) | libpq 18.6, built from the SHA-256-pinned PostgreSQL source tarball and linked statically into `_pq.c`, in **pipeline mode** |
| JSON (default) | `_cjson.c`: one C call writes a whole response |
| gzip (default) | Intel ISA-L 2.32.1 igzip, level 0 (`_igzip.c`); libdeflate 1.26 (`_deflate.c`) where nasm is missing. Both built from SHA-256-pinned source tarballs |
| JWT | stdlib `hmac.digest` (HS256 only), no library |
| Also selectable | uvicorn 0.54.0, granian 2.8.7, FastAPI 0.143.3 (Starlette 1.8.0, Pydantic 2.14.1), asyncpg 0.32.0, orjson 3.13.0 |

`requirements.txt` pins every package, transitive ones included. `install.sh` (root) installs
only Ubuntu packages (Python, a C compiler, make, bison, flex, perl, nasm) and adds no apt sources, so
the shared box keeps its own libpq. `build.sh` (user, ~70 s on a clean box) downloads the pinned PostgreSQL, ISA-L and
libdeflate sources, builds libpq 18 (plain TCP + SCRAM: the database runs with `ssl = off`),
creates the venv, compiles the four extensions, and runs the unit tests. libpq 18 rather than
Ubuntu's 16 because of `PQsendPipelineSync` (see below): with 16's flushing `PQpipelineSync`, a
saturated worker used ~7% more CPU per request and held ~5% fewer requests per second.

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
    inbound/         fast.py, raw.py, uvicorn_server.py, granian_server.py   (servers)
                     asgi_bare.py, fastapi_app.py, rsgi_bare.py     (apps)
    outbound/        postgres/{libpq.py,_pq.c,_fastloop.c,asyncpg_repo.py,pgjson.py}
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
| `FEED_SERVER` | `fast` (needs `FEED_DB=libpq`), `raw`, `uvicorn`, `granian` | who accepts connections |
| `FEED_NATIVE_READS`, `FEED_BATCH_WINDOW_US` | `1`, `2000` | `fast` only: `0` sends every request through Python; the pause after a light pass |
| `FEED_APP` | fast, raw: `raw` · uvicorn: `bare`, `fastapi` · granian: `rsgi`, `bare`, `fastapi` | who turns HTTP into calls |
| `FEED_DB` | `libpq`, `asyncpg`, `pgjson`* | `PostRepository` |
| `FEED_JSON` | `cjson`, `handwritten`, `orjson`, `pydantic`, `pgjson`* | `JsonCodec` |
| `FEED_PROFILE` | off, `latency`, `cprofile`, `latency,cprofile` (not with `fast`) | decorators |
| `FEED_GC` | `survivors`, `generational` | cyclic-GC policy (`runtime/memory.py`) |
| `FEED_GZIP` | `igzip` (if built), `libdeflate`, `zlib` (not with `fast`) | response compressor (`inbound/policy.py`; FastAPI keeps its own middleware) |
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
  CPU quota of its own cgroup and its ancestors, not `os.cpu_count()`, so the 8- and 4-core runs size themselves correctly.
- **libpq pipeline mode, one flush per loop iteration.** Many queries are in flight per
  connection, and Postgres answers them in order, so FIFO order *is* the correlation. Everything
  queued during one loop iteration leaves in one `write(2)`, under one sync point
  (`PQsendPipelineSync`, libpq 17+, which doesn't flush): Nagle's batching without Nagle's delay.
- **Separate write lanes.** A pipelined connection is FIFO, so an INSERT waiting on its WAL flush
  (`synchronous_commit` stays on) would block every read queued behind it. Writes get lanes of
  their own.
- **Lane count** (`domain/capacity.py`): 64 read and 16 write backends in total, whatever the core
  count, with at least 2 write lanes per worker (the cap is 512). Group commit means a write lane's
  throughput grows with its queue, so few are needed.
- **Lanes heal themselves.** A broken connection fails only the requests in flight on it (they're
  not replayed, since a replayed write could commit twice), then reconnects in the background
  with capped backoff, off the event loop. While a lane is down, the repository drops it from
  the round-robin. Measured locally: terminating all 80 backends mid-load cost 0.27% errors over
  the run, and every lane was back within 3 s. Before this, every request routed to a dead lane
  failed for the rest of the run.
- **Rows decoded straight into domain tuples in C.** `timestamptz` arrives as text in a UTC/ISO
  session (session-level `SET`, allowed by rule 2) and is truncated to milliseconds in C. A test
  checks it against the domain's Python reference.
- **A C JSON encoder.** One C call writes the whole feed into one buffer. All four codecs are
  tested byte-for-byte equal on hostile input (quotes, backslashes, control characters, emoji).
- **Hand-written HTTP/1.1.** The three headers the contract reads are found with `bytes.find`
  on a lowercased copy of the head. Connections are `__slots__` Protocol objects, and there's
  no per-connection idle timer (the contract wants ≥ 75 s; never closing idle connections
  satisfies that). HTTP pipelining is answered in order.
- **No Task per request.** The raw adapter steps the use-case coroutine itself: `send(None)` up
  to the repository future, the future's done-callback steps it again, and `StopIteration`
  returns the reply. Measured in isolation: 0.71 µs per request vs 1.45 µs for an eager Task.
  That's about 1% of a request, less than profiling first suggested, because py-spy's
  `ensure_future` share included the use case's first step.
- **gzip level 1, feed only (≥ 1 KB), with a compressor sized for the body.** Single posts and
  errors are too small to be worth the CPU. A default zlib stream allocates ~256 KB of state per
  call, which glibc returns to the kernel on free: a `brk`, fresh page faults and a 64 KB memset
  on every feed response. py-spy put a third of the worker's samples there, and serving the same
  load without gzip measured 57 vs 83 µs per request. A 4 KB window with `memLevel` 4 (~24 KB of
  state) gives the same 764 bytes on the real feed and brought the stack to 69–70 µs per request
  (`FEED_GZIP=zlib`). **libdeflate** (one compressor per worker, allocated once, 738 bytes on the
  same feed) brings it to 60–62 µs. The default, **ISA-L igzip level 0** (stateless, no allocation
  at all, 1,007 bytes on the same feed), brings it to **56–57 µs**. That's three interleaved rounds
  each, and it matches what the same stack cost before with no gzip at all. The 270 extra bytes per
  feed are ~42 Mbit/s at a million users; CPU is the scarcer resource.
- **JWT: one `hmac.digest` + `compare_digest`**, with the signature checked before any JSON is
  parsed. `exp` is required. Nothing is remembered between requests (rule 9).
- **Freeze what survives (`FEED_GC=survivors`, `runtime/memory.py`).** The startup heap is
  frozen, and so is everything that survives each young collection. Without this, every
  keep-alive connection leaves five tracked objects in generation 1, and each gen-1 collection
  rescans all of them. At 100,000 connections in one worker that was a measured **177 ms** stop,
  linear in connections, so ~1.3 s at 750,000 per worker (3M users on 4 cores). With it, the
  same forced collection takes 0.04 ms. Young cycles are still collected, and closed
  connections are freed by refcounting either way (checked by churning 40,000 connections).

## The C read path (`FEED_SERVER=fast`, the default)

The challenge's scoring showed this entry's Postgres box running at 61% at a million users, where most
entries sit at 20–35%. Most of the change below comes from what the top entries had already measured
and written up; credit to them, and it's noted where each idea came from.

**Postgres traffic** (applies to every server using libpq):

- **The feed's `LIMIT 20` is a literal**, as the SPEC writes it. Bound as `$1`, Postgres planned the feed
  on every execution (`go-epoll-trungdlp` found this).
- **A pass's reads share one lane and one Sync**, up to 64, on the least loaded lane
  (`c-epoll-unorig-username`). Each Sync makes Postgres flush and wake, so one per query was the cost.
- **Writes are group-committed**: one transaction in flight per lane, the rest wait and share the next. A write
  is answered only when its Sync has been reached, after the commit. A failed batch rolled back, so each of
  its writes runs again in its own transaction and gets its own answer.
- **64 read + 16 write lanes in total**, with at least 2 write lanes per worker (16 workers: 4 + 2 each).

**The loop** (`_fastloop.c`, linked into `_pq`):

- **Hot reads never enter Python.** `GET /feed` and `GET /posts/<id>`: parse in place, prepared statement,
  binary result straight to JSON, gzip, one `send`.
- **Everything else goes over a bridge** to the same `Router` and use cases as every other adapter: writes, JWT,
  `/health`, all errors, and anything the parser doesn't recognise with certainty (`/posts/00001`, a 19-digit id).
  So the domain stays the only authority on the rules.
- **What this costs the architecture:** the JSON format and the shape of a valid id now exist twice, and the
  core doesn't run for hot reads. `tests/test_fastloop.py` holds the two paths to the same bytes. The core's
  imports are still enforced by `tests/test_architecture.py`.
- **A connection is a 40-byte slot** indexed by fd, with buffers only while a request or response is split.
- **A 2 ms pause after a pass with under 32 events**, unless a lane still has replies to read, so the next pass
  serves more per wake-up. A busy loop never pauses.
- `TCP_NODELAY`, `TCP_DEFER_ACCEPT`, cleared `TCP_QUICKACK` and `TCP_THIN_LINEAR_TIMEOUTS` on the listener.
- The write lanes are the same Python `WriteLane` as `raw`, on a small asyncio-shaped surface of the C loop.

**Measured on a development laptop, not the challenge box.** One worker on one core (Intel Ultra 7 255H E-core),
Postgres on 3 other cores, `mload` on 6, over loopback, igzip, `-think-scale 0.1`, 30 s holds. "Whole-CPU" is
the core's busy time per request from `/proc/stat`, including softirq, which per-process accounting misses:

| | Highest load passing | Next step up | Whole-CPU µs/req at the top load |
|---|---:|---:|---:|
| Before these changes (1220c74) | 13,700 req/s (16k users) | fails at 24k users | 43–57 |
| Now | 32,800–35,400 req/s (48k users) | fails at 56k users, with `mload` near its limit | 23–26 |
| `c-epoll-unorig-username`, same bench | | fails at 48k users (32,200 req/s, p99 1.36 s, one run) | 24 |

- **Capacity is roughly 1.8–2.4× per core**, depending on the load the two are compared at. Above this
  point the laptop's load generator, not the server, runs out first, so these are lower bounds and
  anything beyond is not measured here.
- **Postgres CPU at a fixed 14k req/s:** 187 → 84–107 µs per request.
- **Idle connection memory** (Python side, 12,000 connections into one worker): 1,356 → **40 bytes**.
- **Killing all 80 backends twice under load:** 0.31% errors, all lanes back within seconds.

Loopback charges the load generator's receive work to the server's core, so kernel time is overstated here, and
none of this tests a real NIC or a million connections. Only the challenge's own run can say what this scores.

**Correctness of the C path:** `tests/test_fastloop.py`, with `FEED_TEST_DATABASE_URL` set, runs both servers on
one database and compares their bytes for hostile post bodies, odd ids and paths, auth failures, FK failures in a
batch, pipelining, byte-by-byte split requests and oversized requests. It then checks over 10,000 pipelined
responses from 40 connections, some closing mid-request, for any response crossing over to the wrong request.

## Why this default: measured, not guessed

*This section predates the C read path. It ranks the Python-hosted adapters, on process CPU time.*

The 16-core score is capped at 1,000,000, so the contest is holding a million users with *as
little compute as possible*: the 8- and 4-core scores. A million users is ~41,700 req/s, i.e.
~10,400 req/s per core at 4 cores. So the default is whatever spends the least CPU per request
and the least memory per connection.

Each combination ran as **one worker pinned to one core**, driven by the challenge's own `mload`
(`-gzip`, the real request mix, 6,000 users, `-think-scale 0.1`, ~5,800 req/s offered), on a
fresh copy of the seed each time. The metric is the worker's CPU time per request
(`/proc/<pid>/stat`), two rounds each. The numbers come from a development machine, so they
rank the adapters rather than predict the box:

The "≈ req/s per core" column is *extrapolated* as 10⁶ ÷ (µs per request): at this load the
worker was only ~52% busy. The p99 column is from runs where the local Postgres shared CPUs with
the load generator. Pinned apart, the same default run gives p99 64–96 ms. The measured
saturation figures are in the next table.

| Stack | CPU µs / request | ≈ req/s per core (extrapolated) | p99 |
|---|---:|---:|---:|
| **raw + libpq + cjson** (default) | **91–94** | **~10,600–10,900** | 413–686 ms |
| raw + libpq + handwritten | 92–95 | ~10,500–10,800 | 374–470 ms |
| raw + libpq + pydantic | 90–95 | ~10,500–11,100 | 447–474 ms |
| granian RSGI + libpq + cjson | 105–106 | ~9,400–9,500 | 352–429 ms |
| raw + asyncpg + cjson | 128–130 | ~7,700–7,800 | 1.16–1.17 s ✗ |
| uvicorn + FastAPI + asyncpg + Pydantic | 294–315 | ~3,200–3,400 | 12–13 s ✗ |

These rows were measured before the gzip changes below, which apply to every stack using
`policy.encode`. The default now measures **56–57 µs** per request on the same setup.

- The HTTP layer and the driver dominate. libpq pipelining saves ~28% of the CPU per request
  over asyncpg, and the classic FastAPI stack costs ~3.3× more per request.
- The JSON codec does not matter at this size: the three codecs differ by less than the
  run-to-run noise (one feed encode is ~30 µs of a ~90 µs request). `cjson` stays the default
  because it does the least Python-level work.

**Measured saturation, one worker on one core.** These runs used the default stack with 32 write
lanes, `-think-scale 0.05` and a 15 s ramp. Postgres was pinned to 3 other cores of the same
machine, so these are lower bounds:

| Users | req/s held | worker CPU | p95 | p99 | Result |
|---:|---:|---:|---:|---:|---|
| 8,000 | 12,863 | 76% | 160 ms | 363 ms | pass |
| 12,000 | 16,998 | 83% | 194 ms | 370 ms | pass |

Run to run, the tails vary by about 2×: a 16-write-lane run at 8,000 users gave p95 64 ms and
p99 91 ms.

So one Python worker held ~17,000 req/s. A million users needs ~10,400 req/s per core on 4 cores
and ~5,200 on 8. Before write lanes became a box-wide total, the same worker with 1 write lane
failed at 12,000 users: writes queued for > 10 s while reads stayed under 70 ms.

Memory per idle keep-alive connection (12,000 connections into one worker, RSS delta, kernel
socket memory not included): **raw ≈ 1.3 KB**, granian ≈ 8.9 KB. So a million connections cost
~1.3 GB of Python heap on the default stack, and the 3,000,000-user search on 8 and 4 cores stays
well inside 32 GB.

**Many connections, one worker** (a local harness with 4 loopback addresses × 2
ports to get past the ephemeral-port limit, think time scaled so the load stays ~4,000 req/s):

| Connections | CPU µs / request | worker RSS | per connection | gen-1 + gen-2 collection |
|---:|---:|---:|---:|---:|
| 20,000 | 91 | 55 MB | 1.37 KB | |
| 50,000 | 93 | 95 MB | 1.37 KB | |
| 100,000 | 96–98 | 157 MB | 1.35 KB | 177 + 218 ms (`generational`) → 0.04 + 0.07 ms (`survivors`) |

The cost per request rises ~6% over a 5× increase in connections, and memory per connection
stays flat. All runs passed with 0 errors.

## Rules compliance

These hold for the default configuration:

- The spec's SQL, verbatim, one statement per request (`adapters/outbound/postgres/sql.py`).
- Prepared statements, pipelining, and batching under one Sync. Reads and writes stay one SPEC statement per request.
- No caching of any kind.
- Writes are committed before the response: answered only at their Sync's result, after the commit.
- Every token's signature and `exp` are checked on every request.
- At most 512 connections, enforced at startup.
- Every dependency is pinned: wheels in `requirements.txt`, ISA-L and libdeflate by their
  tarballs' SHA-256, built from source by `build.sh`.
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

- `build.sh` runs 37 unit tests: domain rules, JWT edge cases, the use cases against in-memory
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
