# Python + FastAPI

| | |
|---|---|
| Language | Python 3.12.3 (Ubuntu 24.04's `python3` package) |
| Framework | FastAPI 0.143.3 (Starlette 1.8.0) |
| Server | Gunicorn 26.2.0 with uvicorn 0.54.0 workers (`uvicorn-worker` 0.4.0), uvloop 0.23.0, httptools 0.9.0 |
| Postgres driver | asyncpg 0.32.0 |
| JSON / gzip | orjson 3.12.0 / ISA-L through `isal` 1.8.0 |

Every dependency, transitive ones included, is pinned in `requirements.txt` and installed as a wheel.

## Running it

```bash
sudo bash install.sh   # apt: python3, python3-venv (only if missing)
bash build.sh          # creates .venv and installs requirements.txt
DATABASE_URL=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

Optional: `WEB_CONCURRENCY` (workers, default 1.5 per core) and `DB_POOL` (connections per worker,
default 128 in total).

## The idea

A lot of the fast entries drop the framework and write their own HTTP server. I wanted to see how
far a normal FastAPI stack goes: the FastAPI app, its router and lifespan, uvicorn and Gunicorn,
asyncpg. So the framework and the server stay, and I cut the work each request does that this API
doesn't need. Every change below was profiled first (py-spy, perf, strace), then measured.

## What changed, and what each step bought

"CPU per request" is the `challenge` service's CPU time (systemd `CPUUsageNSec`) divided by the
requests served, under `mload -gzip -think-scale 2.5`. Each step was measured against the step
before it on the same VMs, because two VMs of the same type can differ a lot (see Results).

| Step | Change | CPU per request |
|---|---|---|
| 0 | `examples/python`, unchanged (2 vCPU, 50k users) | 1.82 ms |
| 1 | gzip level 1 instead of `GZipMiddleware`'s level 9 | 1.57 ms |
| 2 | Raw ASGI endpoints, slim middleware stack, orjson, JWT checked by hand | 0.651 ms |
| 3 | Timestamps formatted with `isoformat`, no timeout on `pool.acquire()` | 0.570 ms |
| 4 | No session reset when a connection goes back to the pool, GC tuned, 6 workers instead of 9 | p99 at 120k users on 4 vCPU: 1,245 ms → 5 ms |
| 5 | Timestamps as text, one write per response, ISA-L gzip (4 vCPU, 150k users) | 0.374 → 0.306 ms |
| 6 | A small connection pool instead of asyncpg's (4 vCPU, 300k users) | 0.307 → 0.261 ms |
| 7 | uvicorn's proxy-header middleware off, faster timestamp slicing | 0.261 → 0.249 ms |

The details, in the order of how much they bought:

- **No FastAPI dependency solving, no Pydantic, no Request/Response objects.** `@app.get` makes an
  `APIRoute`, which runs `solve_dependencies` and validation on every request. Here the routes are
  Starlette `Route`s on the FastAPI router, and the endpoints are raw ASGI apps (Starlette mounts a
  non-function endpoint as-is). The middleware stack is the router plus one `try/except` that keeps
  every error in the API's JSON format. I did the same in my
  [$12 server entry](https://github.com/arjaythedev/twelve-dollar-server-challenge/tree/main/submissions/python-fastapi-tejasnafde).
- **gzip level.** The example's `GZipMiddleware` compresses at level 9. Level 1 alone was step 1,
  and step 5 swapped zlib for ISA-L, a faster deflate library, at its fastest level.
- **Timestamps as text.** asyncpg decodes a `timestamptz` into a `datetime`, and then it has to be
  formatted back into a string: about 8% of the CPU in py-spy. The connections set
  `TimeZone=UTC` at connect (a session setting, rule 2), so Postgres sends
  `2026-10-27 12:34:56.123456+00` and the app slices it into `2026-10-27T12:34:56.123Z`. Slicing
  truncates, as SPEC.md asks.
- **A small pool.** asyncpg's `Pool` makes a proxy object and a future on every acquire, and every
  release runs in a new Task (`asyncio.shield`) plus a session reset (`RESET ALL`, unlock advisory
  locks, ...). That's one extra round trip per request unless the reset is turned off. The pool in
  `app/db.py` is a list of connections: acquire is a `pop()`, release hands the connection to the
  oldest waiting request. The app never changes session state, so there is nothing to reset. A
  connection that dies is replaced in the background.
- **One write per response.** `strace` on a worker showed about 3 `write` calls per request. ASGI sends
  a response as two messages, headers then body, and uvicorn writes each one to the socket as it
  arrives, so every response was two system calls and usually two TCP packets. `app/coalesce.py`
  subclasses uvicorn's `RequestResponseCycle` to keep the headers and write them with the body. All
  of uvicorn's own checks still run.
- **GC.** Each worker holds tens of thousands of open connections, and a full collection walks all
  of their objects, which showed up as p99 spikes. After startup the app runs `gc.freeze()` and
  raises the collection thresholds. Requests make almost no reference cycles.
- **Workers.** 1.5 per core. At 300k users on 4 vCPU, 4 workers had a p99 of 1.1 s, and 8 used 6%
  more CPU per request than 6.

## Results

These are from my copy of the rig on Google Cloud, built with `box/setup-app-box.sh`,
`box/setup-db-box.sh` and `box/challenge.service`. The app box is an `n2` VM (Intel Cascade Lake,
2.8 GHz), Postgres 18 runs on its own `n2-standard-16` (or `n2-standard-8` for the 4 vCPU runs),
and the load comes from 4 to 5 `e2-standard-4` VMs running `mload -gzip -think-scale 2.5`. Every
run starts on a fresh copy of the seed. One thing to know: a load generator stalls at about 64,000
connections per server IP (it runs out of source ports), so the app VM got 4 extra IPs, like the
challenge rig has.

| App box | Users | Hold | req/s | p95 | p99 | Errors | App CPU |
|---|---:|---|---:|---:|---:|---:|---|
| 16 vCPU | 1,000,000 | 5 min | 32,560 | 5.3 ms | 13 ms | 0 | about 8 cores |
| 16 vCPU | 800,000 | 2 min | 23,695 | 8.5 ms | 102 ms | 0 | |
| 4 vCPU | 330,000 | 2 min | 12,611 | 6.0 ms | 28 ms | 0 | about 3.2 cores |

I didn't run 8 vCPU. Also, a second `n2-standard-4` I tried later used about 45% more CPU per
request than the first one with the same code, and it passed 200k users but not 330k. Your box is a
Xeon Platinum 8260, so expect the 4 and 8 core numbers to differ from mine. At a million users on
16 cores the app used about half the box, so that one has room.

## Tried, and not kept

- **Python 3.14** (deadsnakes): 0.314 ms per request against 0.306 ms on 3.12.
- **One keep-alive timer per connection** (uvicorn makes and cancels a timer on every request) and
  **calling the prepared statements directly** (skipping asyncpg's statement cache lookup): about
  2% less CPU in one run, a worse tail in the repeat. Not worth the extra code.
- **8 workers on 4 cores**: see Workers above.

## Where the time goes now

py-spy on one worker at 250k users on 4 vCPU: about a quarter of its time is waiting in `epoll`,
the `write` system call is about 6%, and the rest is spread over uvicorn's protocol code, Starlette
routing, asyncpg and the handlers, with nothing above 5%. At 300k users, the kernel's network work
was about 17% of the box's CPU (7% system, 10% softirq). Going further would mean replacing uvicorn or the
framework, which is the thing this entry set out not to do.

## Rules

- One SPEC.md query per request, prepared by asyncpg. No JSON in SQL.
- No caching. The JWT's HS256 signature and `exp` are checked on every request.
- About 128 Postgres connections in total. Session settings only (`TimeZone`, `DateStyle`).
- Serves ports 80 and 81 directly. Nothing on the box is tuned.

## Testing

- `test/test.sh`: `passed=42 failed=0` on the GCP app box under systemd, and with `test/run.sh`
  in a copy of CI (a `postgres:18` container on a 4 vCPU VM).
- Raw-socket checks: `HEAD`, `Connection: close`, HTTP/1.0, three pipelined requests, gzip, a
  malformed request (400).
- Killing all 126 Postgres backends while 400 requests ran: 1 request got a 500 (its query was on a
  killed connection), the other 399 and every request after it got a 200, and the pool reconnected.

## License

MIT, under the repo's [license](../../LICENSE).
