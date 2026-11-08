# python-uvloop-shhossain: uvloop workers, a hand-written HTTP/1.1 protocol, asyncpg

The feed API from `SPEC.md` in CPython. No web framework: one `asyncio.Protocol` per connection on uvloop, asyncpg
for Postgres, orjson for JSON and ISA-L for gzip. At a million users the work is ~42,000 requests per second, but
the server also holds a million open keep-alive connections. So the server is built for two things: an idle
connection costs as little memory as possible, and a request costs as little CPU as possible.

## Stack

| Piece | Version |
|---|---|
| Python | CPython 3.12.3, Ubuntu 24.04's `python3` (+ `python3-venv`) |
| Event loop | uvloop 0.23.0 (libuv) |
| Postgres driver | asyncpg 0.32.0, no TLS |
| JSON | orjson 3.13.0 |
| gzip | isal 1.8.0 (Intel ISA-L) |
| HTTP, routing, JWT | hand-written, standard library only (`hmac`, `base64`) |

`requirements.txt` pins every package (none has dependencies on 3.12). `build.sh` installs wheels only, so nothing
is compiled.

## Design

**One worker process per core, nothing shared.** `python -m feed` is a master that forks one worker per CPU it may
use. That is the affinity mask (taskset, systemd `AllowedCPUs=`), capped by any cgroup v2 CPU quota (systemd
`CPUQuota=`), so 4, 8 and 16 cores need no setting. Each worker is a single-threaded uvloop event loop with its own
`SO_REUSEPORT` listeners on both ports (the kernel spreads new connections over the workers) and its own Postgres
connections. A worker binds its listeners only once its Postgres connections are open. A worker that dies is not
restarted. The master exits when none is left.

**An idle connection is one small object.** Each connection is an `asyncio.Protocol` with seven slots (transport,
pending bytes, busy flag, 100-continue flag, and three for the request waiting on Postgres) and no timer, ~1.5 KB
of RSS per connection with uvloop's transport. uvloop reads every socket into one shared 256 KB buffer
per loop, so an idle connection owns no buffer either. A request is parsed straight out of the bytes object uvloop
hands over: `find` for the head's end, a lowercased copy of the header block for case-insensitive lookups, then
byte comparisons for routing. Requests on one connection are answered in order. While one waits for Postgres,
anything else that arrives on that connection waits in the pending slot. Keep-alive connections are never timed
out (the spec asks for at least 75 s).

**Postgres: a small LIFO pool per worker, prepared statements, one round trip per request.** asyncpg runs one
statement at a time per connection, so each worker keeps its share of the connections (384 in total by default,
split evenly) in a stack: under light load the same few stay hot. Each statement from SPEC.md is prepared once per
connection under a name, and a request runs it with asyncpg's Cython `protocol.bind_execute`. That is what
`PreparedStatement.fetch()` calls, minus four Python wrapper coroutines. Every request is one autocommit statement.
A connection that drops is replaced in the background while the others carry the load.

**No asyncio Task per request.** The router returns the statement to run and a callback. The pool runs asyncpg's
coroutine right away, up to the point where it waits for Postgres, so the query is sent from inside
`data_received`. A done-callback on the future it waits on resumes it, and the callback writes the response. A
request creates no Task (in 3.12 every Task also goes into a Python-level `WeakSet`), no handler coroutine and no
Future of its own. When every connection is busy, queries wait in a FIFO with a 30 s deadline.

**The SQL is SPEC.md's**, one statement per request. There is no batching across requests and no caching of any
kind: every request reads Postgres, and every token's HS256 signature, `alg` and `exp` (and `nbf` if present) are
verified on every request.

**`created_at` comes from Postgres as text.** The session runs with `TimeZone=UTC` and `DateStyle=ISO`, and a
custom asyncpg codec asks for `timestamptz` (only) in text form: `2025-12-31 23:59:19.409123+00`. Turning that into
`2025-12-31T23:59:19.409Z` is string slicing (Postgres drops trailing zeros of the fraction, which the slicing
handles), about 6x cheaper than decoding to a `datetime` and calling `isoformat`. I checked it against Postgres's
own `to_char(... 'MS')` on 20,000 random rows and on edge cases. It truncates, like the spec asks.

**JSON** is orjson over dicts built in the spec's key order. Error and like bodies are byte templates.

**gzip** is ISA-L level 0 for bodies of 1 KB or more when the client accepts gzip: `/feed` goes from 5.1 KB to
~2.3 KB. That is the cheapest gzip I measured, ahead of libdeflate and zlib. Single posts go out uncompressed.

**GC.** Automatic collection is off. Each worker collects once a second and then calls `gc.freeze()`, which moves
whatever survived (open connections, mostly) into the permanent generation. No collection ever walks the hundreds
of thousands of connection objects again. Objects that die later are still freed by reference counting.

**HTTP/1.1**: keep-alive, pipelined requests answered in order, `Content-Length` bodies up to 64 KB,
`Expect: 100-continue`, `Connection: close`, HTTP/1.0 keep-alive. Chunked request bodies get a 400 (nothing sends
them). Responses carry `Content-Type` and `Content-Length`, plus `Content-Encoding: gzip` and
`Vary: Accept-Encoding` when compressed.

## Two asyncpg details that matter here

- **Prepare with a name.** With `statement_cache_size=0`, `Connection.prepare()` defaults to the *unnamed*
  statement, which asyncpg re-sends (Parse) with every execution. Postgres then parses and plans every query. On a
  16-core database box that capped the whole system at ~25,000 req/s with Postgres at 100% CPU. With named
  statements the same test ran at ~42,000 req/s with Postgres about half busy.
- **Close the transaction `prepare()` leaves open.** asyncpg prepares with Parse/Describe/Flush and no Sync, and
  Postgres opens an implicit transaction at the Parse. Left open, the first statement on that connection runs in
  it, so an `INSERT` minutes later gets `created_at = now()` from when the connection was set up. One
  `SELECT 1` after preparing ends it.

## Settings

All optional. The defaults are what gets benchmarked.

| Env | Default | |
|---|---|---|
| `WORKERS` | CPUs available | worker processes |
| `DB_CONNS` | 384 | Postgres connections in total, split evenly over the workers (at most 512) |
| `GZIP_LEVEL` | 0 | ISA-L level 0-3 |
| `GZIP_MIN` | 1024 | smallest body that is gzipped |

## Running

```bash
sudo ./install.sh   # python3 + python3-venv from Ubuntu, if missing
./build.sh          # .venv with the pinned wheels
DATABASE_URL=postgres://user:pass@host:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 ./start.sh
```

## Measured (not on the benchmark boxes)

On a GCP copy of the setup: the app box is an `n2-custom` VM with 32 GB on Intel Cascade Lake (the scoring box's
Xeon Platinum 8260 is the same generation, at 2.4 GHz instead of 2.8), set up with `box/setup-app-box.sh` and
nothing else, and the server is started by `box/challenge.service`. Postgres 18 runs on a separate 16 vCPU VM set
up with `box/setup-db-box.sh`. Load comes from four to six 16 vCPU load generators running `mload -gzip
-think-scale 2.5` against the app box's address plus 16 alias IPs on ports 80 and 81, ramping in at 3,300 users/s.
Fresh database for every run.

| App box | Users | Hold | req/s (hold) | p50 | p95 | p99 | Errors | App CPU | Box CPU | App memory |
|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 16 vCPU | **1,000,000** | 5 min | ~42,300 | 1.2 ms | 5.1 ms | 9.2 ms | 0 | 5.4 cores | ~41% | 5.2 GB |
| 8 vCPU | 1,200,000 | 2 min | ~51,000 | 1.4 ms | 14.1 ms | 23.0 ms | 0 | | | |
| 8 vCPU | **1,300,000** | 5 min | ~55,000 | 1.8 ms | 12.7 ms | 19.6 ms | 0 | 7.1 cores | ~93% | 6.7 GB |
| 8 vCPU | 1,500,000 | 2 min | ~57,000 (saturated) | | 3.4 s | 3.8 s | 0 | 7.8 cores | 100% | 7.8 GB |
| 4 vCPU | 600,000 | 2 min | ~25,400 | 1.4 ms | 5.3 ms | 8.7 ms | 0 | 3.2 cores | ~85% | 3.1 GB |
| 4 vCPU | **680,000** | 5 min | ~28,800 | 2.5 ms | 32.6 ms | 78.3 ms | 0 | 3.7 cores | ~95% | 3.5 GB |

So on that hardware: 1,000,000 (the 16-core cap) on 16 vCPUs, about 1.3 million on 8 and about 680,000 on 4. On
8 and 4 vCPUs the server runs out of CPU: about 140 µs per request in all, of which the kernel's network work
(system time and softirq) is a third. The scoring box's CPU runs at a lower clock, so expect its numbers to come
out somewhat lower.

"req/s" and the percentiles are over the whole run, ramp included, as `mload` reports them. App CPU and memory are
the app's cgroup (`challenge.service`), and memory includes the kernel's socket memory charged to it: about
5 KB per open connection in all. Box CPU includes the kernel's network processing (softirq), which is ~22% of
the 8 vCPU box at 1.3M users.

Per request at 42k req/s on 16 vCPUs: ~114 µs of app CPU (user ~86, system ~28) plus ~30 µs of softirq.

- `test/run.sh`: `passed=42 failed=0`, on macOS, on Ubuntu 24.04 in Docker (10 workers, three runs in a row) and on
  the GCP app box under systemd.
- HTTP edge cases (my own raw-socket script): pipelining across async and immediate responses, byte-by-byte
  requests, gzip negotiation including `q=0`, `Connection: close`, HTTP/1.0 with and without keep-alive,
  `Expect: 100-continue`, split bodies, chunked bodies (400), oversized heads (431), clients that disconnect while
  their query runs.
- Killing every Postgres backend mid-load: each worker logs the loss, reconnects, and keeps serving.

## What didn't help

- **Python 3.14.10** (built from the python.org tarball with PGO + LTO): 116 µs of app CPU per request against
  114 µs on Ubuntu's 3.12, measured the same way. Most of the time is in C (uvloop, asyncpg, the kernel), so a
  faster interpreter barely shows. It isn't worth a 12-minute source build in `install.sh`.
- **zlib and libdeflate for gzip**: both slower than ISA-L level 0 on the 5 KB feed (zlib level 1 about 2.5x,
  libdeflate level 1 about 2x), for a slightly smaller body.
- **Decoding `timestamptz` to `datetime` and calling `isoformat`**: about 6x the cost of reformatting Postgres's
  text output, 20 times per feed.
