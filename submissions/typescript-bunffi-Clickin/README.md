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
| Event loop | own epoll loop per worker via `bun:ffi` (libc), `SO_REUSEPORT` + `TCP_DEFER_ACCEPT` listeners on both ports |
| Socket I/O | a pass's client `recv`s and `send`s batched through io_uring, set up with raw system calls (no liburing) |
| HTTP | hand-written HTTP/1.1 parser and response writer (`src/http.ts`, `src/loop.ts`) |
| Postgres | hand-written wire protocol v3 client (`src/pg.ts`): SCRAM-SHA-256/MD5/cleartext login, prepared statements, binary parameters and results |
| JWT | HMAC-SHA256 written in TypeScript over `Int32Array` (`src/jwt.ts`), checked against `node:crypto` at startup; header and claims read by a small byte scanner, with `JSON.parse` deciding anything it doesn't take |
| JSON | responses written byte by byte into a buffer (`src/json.ts`); `JSON.parse` only for `POST /posts` bodies |
| gzip | ISA-L igzip level 0 (`libisal2` from apt) through `bun:ffi`, for bodies of 1 KB or more |
| Build | `bun build --minify` into `dist/server.js` |

## Process model

- **One worker process per CPU** (`nproc`, so it follows the affinity mask). The primary only starts the workers
  and restarts any that die; a worker exits when its parent is gone. With one CPU there is no primary.
- **Every worker listens on both ports with `SO_REUSEPORT`**, and owns its client connections and its Postgres
  connections in one epoll set. A request never leaves the worker that accepted it.
- **The loop thread runs under `SCHED_IDLE`**, the lowest scheduling class, as in `c-epoll-unorig-username`: on
  the same core anything else runs first, above all `ksoftirqd`, which does the receive work once the kernel defers
  it under load (the box's NIC has one queue, so that work lands on one core). It is the thread's own policy: no
  affinity, nothing raised, nothing box-wide. Bun's helper threads keep the normal class.
- **The loop is synchronous.** Once it starts, no Promise, timer or Bun I/O is used, so the JavaScript thread only
  ever runs the loop: `epoll_wait`, read, handle, flush Postgres, send, repeat.

## System calls

The scheme of `c-epoll-unorig-username` (#1), in TypeScript (`src/ring.ts`):

- **epoll still says which client sockets are ready, but their reads and writes are queued as io_uring RECV and
  SEND operations** and submitted together: a pass enters the kernel once for all its reads and once for all its
  responses, instead of one `recv` and one `send` per request. That also removes the two FFI calls per request.
- **Every operation carries `MSG_DONTWAIT`**, so it finishes inside `io_uring_enter`, and its result (bytes,
  `-EAGAIN`, an error) is handled exactly as a `recv`/`send` result was. A partial send keeps its tail and waits for
  `EPOLLOUT`, as before.
- **The ring is set up with raw system calls** (`io_uring_setup`, `mmap`, `io_uring_enter` through libc's
  `syscall`) and its queues are read and written through typed arrays over the mapped memory. No liburing.
- **Safety rules:** at most one queued send per connection (a later response in the same pass waits behind it, so
  bytes never change order); queued operations are submitted before any descriptor is closed, so a send cannot
  reach a descriptor number that `accept4` gave to someone else; a queued operation's bytes live in a per-batch
  slab that is not reused until its results are handled.
- **Fallback:** when `io_uring_setup` fails (an old kernel, or a seccomp profile like Docker's default, which
  answers `EPERM`), the worker logs it once and uses plain `recv`/`send`. `NO_URING=1` forces that path.
- **`TCP_DEFER_ACCEPT`** on the listeners: a connection is accepted once its first request is there, and it is read
  right after `accept4` instead of a pass later.

## Memory and allocation

- **An idle connection costs a few bytes:** a generation number and a flag byte in typed arrays indexed by fd. It
  owns no buffer and no object. A connection gets a buffer only while it holds a partial request or an unsent
  response tail.
- **Requests are parsed in place:** the parser leaves offsets in module-level variables, and nothing on the read
  path creates a string.
- **Responses are built in one buffer per worker**, gzipped into a second one, and copied into the batch's send
  slab. Rows from
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
- **No kernel or host changes.** The loop thread's own scheduling class (`SCHED_IDLE`) and its own sockets'
  options are the only settings it makes. `EXIT_AFTER_S`, when set, makes a worker exit after that many seconds, so
  `bun --cpu-prof` can write a profile. It is unset in `start.sh`.

## Local measurements

Measured on an arm64 Mac (OrbStack containers), with the server on **one core** in a container without a seccomp
profile (the benchmark box's service has none either; Docker's default profile blocks io_uring and adds a cost to
every system call). `bench/mload -gzip`, think time scaled down. Absolute numbers don't carry over to the
benchmark box; the ratios are the point. Each row is the mean of two or more alternating runs.

CPU at a fixed load (16,000 users, `-think-scale 0.05`, about 33,000 req/s):

| Build | server CPU | user | system |
|---|---:|---:|---:|
| previous version (PR #98: libdeflate, plain `recv`/`send`) | 0.46 core | 0.26 | 0.20 |
| + ISA-L level 0 instead of libdeflate level 1 (in a seccomp'd container: 0.570 -> 0.478) | -16 % | | |
| this version, io_uring off (`NO_URING=1`) | 0.37 core | | |
| **this version** | **0.36 core** | **0.155** | **0.21** |
| `c-epoll-unorig-username`, for reference | 0.25 core | 0.07 | 0.17 |

All of the gain is user time: the FFI calls per request and gzip. The kernel's share (TCP) is unchanged.

Throughput with the server capped at 0.3 CPU, users raised in steps of 1,000 (10,000 to 15,000): the previous
version levels off at about 17,200 req/s (p95 456 ms at 15,000 users), this one at about 21,200 req/s (p95 225 ms).

- **gzip:** ISA-L level 0 makes `/feed` 1,003 bytes instead of libdeflate's 733 (3,656 plain), but the previous
  version and this one send the same number of TCP segments per request (1.793 at `-think-scale 0.5`), and the top entries on the board use the
  same setting. ISA-L level 1 cost more CPU than libdeflate here.
- **`SCHED_IDLE`** made no difference locally, either way. What it is for (`ksoftirqd` competing with a busy loop
  for the core that takes the NIC's interrupts) can't happen in this setup, so its effect is unmeasured.
- Tried and dropped: `TCP_QUICKACK` 0 (no change in segments per request), one GC marker thread
  (`BUN_JSC_numberOfGCMarkers=1`, slightly more CPU).

## Run it

```bash
sudo bash install.sh
bash build.sh
DATABASE_URL=postgres://user:pass@host:5432/feed JWT_SECRET=million-users-challenge HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

`test/run.sh`: **passed=42 failed=0** (Ubuntu 24.04 aarch64 container, Postgres 18), with io_uring and with
`NO_URING=1`. 
