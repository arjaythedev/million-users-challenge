# typescript-bunffi-Clickin: TypeScript on Bun, its own epoll loop through `bun:ffi`

The feed API (`SPEC.md`) in TypeScript, with no web framework, no Postgres driver and no npm packages. Bun is
only the JavaScript engine (JavaScriptCore's JIT) and the FFI: each worker calls libc's `epoll_wait`, `accept4`,
`recv` and `send` directly and runs its own synchronous event loop. HTTP/1.1, the Postgres wire protocol (SCRAM
included), HS256 and the response JSON are written here in TypeScript, over preallocated byte buffers.

`Bun.serve` and `Bun.SQL` are not used. The aim is to stay out of the runtime's per-request costs (an object,
strings and Promises per request) and work on bytes, the way the C, C# and Go entries at the top do.

## Stack

| Piece | Version / how |
|---|---|
| Runtime | Bun 1.4.4, the official `bun-linux-x64.zip` (`aarch64` for local runs), SHA-256 pinned in `install.sh` |
| Event loop | own epoll loop per worker via `bun:ffi` (libc), `SO_REUSEPORT` listeners on both ports |
| HTTP | hand-written HTTP/1.1 parser and response writer (`src/http.ts`, `src/loop.ts`) |
| Postgres | hand-written wire protocol v3 client (`src/pg.ts`): SCRAM-SHA-256/MD5/cleartext login, prepared statements, binary parameters and results |
| JWT | HMAC-SHA256 written in TypeScript over `Int32Array` (`src/jwt.ts`), checked against `node:crypto` at startup |
| JSON | responses written byte by byte into a buffer (`src/json.ts`); `JSON.parse` only for JWT header/claims and `POST /posts` bodies |
| gzip | libdeflate level 1 (`libdeflate0` from apt) through `bun:ffi`, for bodies of 1 KB or more |
| Build | `bun build --minify` into `dist/server.js` |

## Process model

- **One worker process per CPU** (`nproc`, so it follows the affinity mask). The primary only starts the workers
  and restarts any that die; a worker exits when its parent is gone. With one CPU there is no primary.
- **Every worker listens on both ports with `SO_REUSEPORT`**, and owns its client connections and its Postgres
  connections in one epoll set. A request never leaves the worker that accepted it.
- **The loop is synchronous.** Once it starts, no Promise, timer or Bun I/O is used, so the JavaScript thread only
  ever runs the loop: `epoll_wait`, handle the events, flush Postgres, repeat.

## Memory and allocation

- **An idle connection costs a few bytes:** a generation number and a flag byte in typed arrays indexed by fd. It
  owns no buffer and no object. A connection gets a buffer only while it holds a partial request or an unsent
  response tail.
- **Requests are parsed in place:** the parser leaves offsets in module-level variables, and nothing on the read
  path creates a string.
- **Responses are built in one buffer per worker**, gzipped into a second one, and sent from there. Rows from
  Postgres are decoded from the receive buffer straight into the response JSON: ids, timestamps and counts are
  formatted with integer arithmetic, and text columns are copied with JSON escaping.
- **Requests waiting on Postgres are pooled objects**, reused.
- **FFI calls take plain numbers.** Buffers are allocated once and their addresses taken once. Functions return
  `i32` even when C returns `ssize_t`, because a 64-bit return would come back as a BigInt.

## Postgres

The structure follows `csharp-epoll-jh-d` (MIT): the same SQL handling, batching and group commit, ported to
TypeScript.

- **SPEC.md's statements, verbatim**, prepared once per connection under one-letter names. Each request runs
  exactly one of them.
- **Batching window:** a loop pass starts at most every 2 ms. The pass's requests are collected, then dealt in
  contiguous runs of about 16 to the least loaded connections. Each run is Bind + Execute per request, then one
  `Sync` and one `send()` per connection.
- **Reads and writes use separate connections:** 64 and 16 in total, split over the workers.
- **Group commit:** Postgres runs each batch (one `Sync`) as one implicit transaction. A write's response waits
  for the batch's `ReadyForQuery`, which comes after the commit.
- **Reads are answered as soon as their whole result is buffered.**
- **Errors:**
  - A like on a missing post fails with SQLSTATE 23503, which becomes 404. The batch's other writes were rolled
    back with it and are submitted again, along with the requests Postgres skipped.
  - A lost connection: writes whose fate is unknown get a 500. Everything else goes to another connection, and
    the lost one reconnects every second.
  - A connection that owes answers and stays silent for 10 s is dropped.
- **A stats line per worker every 10 s:** connections, answers per second, requests per `Sync`.

## Rules

- **Rule 3:** each request runs one SPEC.md statement, unchanged. Nothing is built in SQL.
- **Rule 5:** 80 Postgres connections in total on any core count.
- **Rule 7:** nothing is cached and nothing about a token is remembered. Kept between requests: connections,
  prepared statements, the HMAC key pads' compressed states, and the current second's `Date` header.
- **Rule 8:** a write's response leaves only after its commit. `synchronous_commit` is untouched.
- **Rule 9:** HS256 signature and `exp` (and `nbf` when present) are checked on every request.
- **No kernel or host changes.** `EXIT_AFTER_S`, when set, makes a worker exit after that many seconds, so
  `bun --cpu-prof` can write a profile. It is unset in `start.sh`.

## Local measurements

These were measured on an arm64 Mac (OrbStack containers, `bench/mload -gzip`, think time scaled down), with the
server on **one core**. Absolute numbers don't carry over to the benchmark box; the ratios are the point.

| Server | req/s | server CPU | CPU per request |
|---|---:|---:|---:|
| `c-epoll-unorig-username` | 33,200 | 0.35 core | 10.5 µs |
| this entry | 33,400 | 0.46 core | 13.8 µs |
| `c-epoll-unorig-username` | 46,300 | 0.57 core | 12.3 µs |
| this entry | 49,200 | 0.79 core | 16.1 µs |
| `typescript-bun-tbsvttr` (`Bun.serve` + `Bun.SQL`) | 16,900 | 0.57 core | 34 µs |
| this entry | 16,800 | 0.30 core | 18 µs |

A CPU profile under load shows the busy time going mostly to native work: the `send()` syscall is about a third
and libdeflate about a fifth. The C entry gzips with ISA-L level 0, which costs less CPU but produces bigger bodies.
On the benchmark box the network is the limit, so this entry keeps libdeflate level 1, as `csharp-epoll-jh-d` does.

## Run it

```bash
sudo bash install.sh
bash build.sh
DATABASE_URL=postgres://user:pass@host:5432/feed JWT_SECRET=million-users-challenge HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

`test/run.sh`: **passed=42 failed=0** (Ubuntu 24.04 aarch64 container, Postgres 18). 
