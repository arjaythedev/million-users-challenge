# tex-cgi-Emsii3: the whole API in pdfTeX, served by a pool of long-lived `pdftex` workers

The feed API from `SPEC.md`, except the application logic is written in TeX. Request parsing, routing, JWT
verification (SHA-256, HMAC and base64url included, as pure TeX macros), JSON parsing of the POST body, JSON
building, timestamp formatting, validation and error mapping all live in `tex/*.tex`. Think FastCGI / PHP-FPM, but
the application server is a typesetting engine: a pool of `pdftex` processes, each looping over requests (v1 and
v2 started a fresh `pdftex` for every request).

This is a joke with a working test suite (`test/run.sh`: `passed=42 failed=0`, golden files byte for byte). It
is **not** going to hold a million users. See "Measured" for how far it does get.

Shoutout to @IdkWhatToWriteHereOhManItWasAlreadyUsed and the CMake submission
([#17](https://github.com/arjaythedev/million-users-challenge/pull/17)), which proved that a build system makes a
perfectly fine web framework. This one is directly inspired by it: same CGI-per-request idea, a different tool
that was never meant for this.

## Updates

Each version is measured the same way (see "Measured": `ubuntu:24.04` container, 10 cores, `ab -k`, 8 s per run,
concurrency 32, request files on `/dev/shm`). Requests per second, p95 in brackets. Run-to-run noise is about
10 to 15 % for reads and more for likes (see "Measured"); `/posts/1` is small, `/feed` has 20 posts to format
in macros, and a like waits for a Postgres commit:

| Version | Change | `GET /posts/1` | `GET /feed` | `POST /posts/2/like` |
|---|---|---|---|---|
| v1 | `pdftex` per request; TeX runs `sh` + a libpq CLI (`pgq`) through `-shell-escape`, a new Postgres connection per request | 529 (94 ms) | 437 (113 ms) | 213 (240 ms) |
| v2 | pooled persistent libpq connections in the host; TeX talks to them through a FIFO pair (`db.req` / `db.resp`); no shell, no `pgq`, no `-shell-escape` | 1013 (28 ms) | 851 (31 ms) | 200 (173 ms) |
| v3 | `epoll` event loop; persistent `pdftex` workers ("TeX-FPM"): one process serves thousands of requests, fed through a `job` / `done` FIFO pair | 16500 (2 ms) | 2650 (13 ms) | 230 (150 ms) |

## Stack

| Piece | Version |
|---|---|
| TeX engine | pdfTeX from Ubuntu's `texlive-binaries` + `texlive-base` (dev machine: pdfTeX 3.141592653-2.6-1.40.29, TeX Live 2026), run in e-TeX mode |
| Host | C99/POSIX, one event loop (`epoll` on Linux, `poll()` elsewhere); built with `gcc -O2`, linked against libpq |
| DB driver | libpq from Ubuntu's `libpq-dev`, a pool of persistent connections inside the host |
| Postgres server | 18 (separate box) |
| Lua / Python / openssl / any language runtime | none |

No Lua (`luatex` is not installed), no `openssl`, and TeX never shells out for crypto. TeX executes nothing: no
shell, no `-shell-escape`; its only I/O is plain file reads and writes in the request directory.

## Design

Two pieces, each with a strictly limited job:

```
client --HTTP--> bin/host (C + libpq)  --job.fifo / done.fifo-->  N x pdftex -fmt=feed '\feedloop'
                  epoll loop, framing,  <--- db.req / db.resp --->   (long-lived; all the logic, JWT,
                  Postgres pool, queue                                JSON, routing, one request at a time)
```

**`host/host.c`: sockets, HTTP framing, the worker pool and the database pool.** Listens on `PORT` and
`EXTRA_PORT`, keep-alive (idle timeout 120 s, the spec asks for 75 s or more), reads the request head and a
`Content-Length` body (max 16 KB), and answers in order on each connection. The event loop is `epoll`
(level-triggered) on Linux and `poll()` on other systems (macOS builds and passes the tests with it), behind a small
`ev_add` / `ev_mod` / `ev_del` / `ev_wait` layer; listeners, client sockets, FIFOs, libpq sockets and the signal
self-pipe all go through it, so an idle connection costs nothing per loop round. An idle keep-alive connection is
one small struct and a socket (no buffers).

At start the host spawns `WORKERS` `pdftex` processes, each with its own control directory `<tmp>/texcgi.<pid>/w<N>/`
(`$TMPDIR`, else `/dev/shm`, else `/tmp`) as its cwd. The same format and flags as before, no `-shell-escape`.
A complete request is queued; as soon as a worker is idle, the oldest queued request goes to it (one request per
worker at a time; a request that waits more than 60 s for a worker gets a 500). The protocol, in the worker's directory:

1. The host writes `meta.txt` (current time, server start time, `JWT_SECRET`), `req.txt` (request line and
   headers) and `body.txt` (the body bytes), `mkfifo`s a fresh `db.req` and `db.resp` and opens them (below).
2. It writes one line to `job.fifo` (the path prefix of the request files; empty here because the worker's cwd is
   the directory; the feed code supports any prefix). TeX sits in `\readline` on `job.fifo`, which blocks until
   the line arrives. The host keeps the writing end open for the worker's lifetime, plus a keeper reader, so TeX's
   `\openin` never blocks and EOF on `job.fifo` (host closes it) is the "exit" signal.
3. TeX runs the request (parse, route, JWT, at most one database command through `db.req` / `db.resp`, JSON),
   writes `resp.txt` (status code on line 1, the JSON body on line 2), then opens `done.fifo`, writes `ok` and closes
   it. (`\write` is buffered in a C `FILE*` and only reaches the FIFO on close, so it has to be open, write, close
   every time.) The host holds `done.fifo` read-write, so it never sees EOF, and collects a complete `ok\n` line.
4. The host reads `resp.txt`, relays it with `Content-Length`, deletes the request files and the `db.*` FIFOs,
   and marks the worker idle (or retires it, see below). If the worker produced nothing usable, or died, the client
   gets `500 {"error":"internal server error"}`. The host does not know what a route, a token or a post is.

A worker that dies mid-request (crash, `kill`, `\end`) is reaped by `SIGCHLD` through the self-pipe: that request
gets the 500 and a fresh worker is started (a worker that dies within a second of starting, for example because
`pdftex` is missing, is retried every 500 ms instead of in a tight loop). A request still running after 30 s
gets its worker killed (500, respawn).

**`tex/feed.tex` + `tex/sha256.tex`: the application.** They are compiled once, at build time, into a
dumped format (`bin/feed.fmt`, ~210 KB). `\feedloop` is the entry point: open `job.fifo`, then forever read a line,
run one request, signal done. It is tail-recursive (macro expansion, never nested), so the input stack does not
grow; the request is the old `\feedmain` logic with every file name prefixed by the line from `job.fifo`. A request
ends in `\feed@respond`, which skips the rest of the handler, closes the groups and conditionals that were open at
that point and goes back to the loop. Things that had to be done by hand in macros: SHA-256 on 16-bit halves (TeX
integers are signed 31-bit), HMAC, base64url decode/encode, a JSON string decoder (`\uXXXX` and surrogate pairs
included), JSON escaping, UTF-8 character counting for the 500-character limit, and Postgres timestamp to
`...T...Z` truncated to milliseconds. Ids and counts are carried as digit strings and never put through
`\numexpr`, so they can exceed 2^31.

**Isolation between requests.** A worker keeps its code, not its data:

- Each request runs inside `\begingroup ... \endgroup`; every local assignment (registers, macros, the parsed request,
  the token, the body) is undone when the group closes.
- The few globals (`\hmacresult`, `\bdecoded`, `\sharesult`, `\ifbvalid`, the JSON parser's error flag, character
  counter, the per-key type and value slots and the "auth header seen" macro) are reset at the start and at the end of
  every request, so nothing is read that was not written by the same request.
- No control sequence name is made from request data: the JSON parser only stores the five keys it knows
  (`\ifcsname` guards the rest), path segments beyond the fourth are ignored, and the lookup tables are built when
  the format is built. The hash table and the string pool therefore do not grow (checked in the log, below).
- Files and streams of a request are opened and closed by that request; the response is written fresh each time.

**Recycling.** `pdftex` itself leaks a little with every file it opens (about 0.6 to 0.9 KB per request,
measured: RSS 17 MB to 51 MB over 30,000 requests, while TeX's own memory counters stayed flat), so after
`WORKER_MAX_REQUESTS` requests (default 5000, about 4 MB of growth) an idle worker is retired (the host closes
`job.fifo`, TeX leaves its loop on EOF) and replaced by a fresh one. Starting one costs about 5 ms, once per 5000 requests.

**The database: a pool in the host, reached through two FIFOs.** The host keeps `DB_POOL` persistent libpq
connections, driven asynchronously from the same loop (`PQconnectStart`/`PQconnectPoll`, non-blocking
sends, `PQconsumeInput`/`PQgetResult`), with the five SPEC statements prepared once per connection and the
session set to `TimeZone=UTC`, `DateStyle=ISO`. One query is in flight per connection; further commands wait in a
FIFO queue. In each request directory the host opens `db.req` and `db.resp` before it hands over the job:

1. If the route needs the database, TeX writes one line to `db.req` with `\immediate\openout`/`\write`/`\closeout`:
   `health`, `feed`, `post <id>`, `create <uid> newbody.txt` or `like <uid> <postid>`. (The body of a new post is
   the file `newbody.txt` in the request directory, one trailing newline stripped.) Routes that need no
   database (404, 400, 401...) never write anything.
2. The host reads the line, queues it, runs the matching statement on the next free connection, and writes
   the answer into `db.resp`, handling partial writes when the pipe is full (a feed is ~20 KB, more than a small
   pipe buffer), then closes its end.
3. TeX `\openin`s `db.resp` and `\readline`s until end of file. The lines are the same as before:
   `OK`, `P|id|created_at|hex(author)|likes|hex(body)`, `C|id|created_at`, `L|1`/`L|0`, and
   `ERR|<sqlstate>|<hex message>` (strings are hex, so no escaping problem can reach TeX).

The host opens `db.req` read-write (so it is also a writer and never sees a hangup) and `db.resp` with a
reader it keeps until the request ends (so an answer written before TeX has opened the FIFO is not lost, and
TeX still gets end of file when the host's writer closes). If a connection breaks the host reconnects with
backoff (100 ms doubling to 2 s); a command that finds no usable connection, or waits more than 10 s for one,
gets `ERR|CONN|...`, which is what makes `/health` answer 503 while Postgres is down and 200 again as soon as it
is back, without a restart. If the worker dies while its query is running, the result is discarded when
it arrives and the connection goes back to the pool clean.

## Rules

- **SQL is SPEC.md's, verbatim, one statement per request.** The post `SELECT` (with `ORDER BY ... LIMIT 20`
  for the feed, or `WHERE p.id = $1`), `INSERT INTO posts ... RETURNING id, created_at`, and
  `INSERT INTO likes ... ON CONFLICT DO NOTHING`; parameters go through prepared statements (`PQsendQueryPrepared`). `/health` runs `SELECT 1`.
- **No caching of any kind (rule 7).** A worker is long-lived, but it keeps code, not data: every request runs in
  its own TeX group, the few globals are reset before and after it, and nothing is derived from an earlier request
  (see "Isolation between requests"). The host keeps no application data either: it relays rows and nothing else.
  Every request reads Postgres, one query per request (at 18,000 `/posts/1` per second, Postgres counted 18,000
  commits per second). No JWT verification is remembered: the HMAC is recomputed every time.
- **The JWT is fully verified on every request**: three parts, header with `"alg":"HS256"`, HMAC-SHA256
  signature recomputed in TeX and compared, `exp` against the current time, `sub` a positive integer string,
  `username` a string. Auth is checked before the post id on `POST /posts/:id/like`, as in the spec.
- **Connections:** the host holds `DB_POOL` persistent Postgres connections (default min(2 x cores, 64),
  clamped to 1..512) and shares them between requests, which rule 7 allows. A request still runs exactly one
  SPEC statement; the pool only removes the per-request connect (SCRAM and a backend fork), not the query.
  Concurrency is bounded by `WORKERS` (default 2 x cores); more requests than workers queue in the host, and more
  database commands than connections queue there too.
- Both ports are served by the same process. No compression (the spec says it is optional).

## Settings

| Env | Default | |
|---|---|---|
| `WORKERS` | 2 x online cores | number of long-lived `pdftex` workers (clamped to 1..512). `MAX_CHILDREN`, the v2 name, is still read as an alias when `WORKERS` is unset |
| `WORKER_MAX_REQUESTS` | 5000 | requests after which a worker is replaced by a fresh one (0 = never) |
| `DB_POOL` | min(2 x online cores, 64) | persistent Postgres connections (clamped to 1..512) |
| `TMPDIR` | `/dev/shm` if writable, else `/tmp` | where the workers' directories go |
| `KEEP_WORKDIR` | unset | `1` keeps the workers' `pdftex` logs in the work directory (debugging) |

## Running

```bash
sudo ./install.sh   # gcc, libpq-dev, texlive-binaries, texlive-base (apt)
./build.sh          # bin/host and the format bin/feed.fmt
DATABASE_URL=postgres://user:pass@host:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 ./start.sh
```

## Measured (not on the benchmark boxes)

An `ubuntu:24.04` container (Docker Desktop on a laptop, 10 cores, so `WORKERS` = 20 and `DB_POOL` = 20) with the
packages from `install.sh`, Postgres 18 in a second container (`max_connections=600`), `ab -k` in the same
container as the server (it takes CPU from it), a fresh copy of the seed database, 8 s per run, request files on
tmpfs (`/dev/shm`, the default), no non-2xx responses (`/like` returns 201 for the first like and 200 after that).
Requests per second, with p95 latency in brackets. v2 is the previous design (a fresh `pdftex` per request), v3 is
the one described above, both measured in the same session:

| Concurrency | Endpoint | v2 | v3 |
|---|---|---|---|
| 1 | `GET /feed` | 156 req/s (7 ms) | 693 req/s (2 ms) |
| 1 | `GET /posts/1` | 194 req/s (6 ms) | 4,669 req/s (0 ms) |
| 1 | `POST /posts/2/like` (authed) | 47 req/s (26 ms) | 67 req/s (16 ms) |
| 8 | `GET /feed` | 720 req/s (17 ms) | 2,721 req/s (3 ms) |
| 8 | `GET /posts/1` | 1,021 req/s (10 ms) | 15,428 req/s (1 ms) |
| 8 | `POST /posts/2/like` (authed) | 237 req/s (38 ms) | 155 to 270 req/s (31 to 58 ms) |
| 32 | `GET /feed` | 854 req/s (36 ms) | 2,650 req/s (13 ms) |
| 32 | `GET /posts/1` | 1,007 req/s (29 ms) | 16,500 req/s (2 ms) |
| 32 | `POST /posts/2/like` (authed) | 227 req/s (149 ms) | 230 req/s (150 ms) |

The v3 c=32 numbers are the median of four runs (feed 1,700 to 2,830, post 14,600 to 17,200, like 178 to 264);
the container shares its CPUs with `ab` and Postgres, so expect 10 to 15 % noise on reads.

Why `/posts/1` is so much faster than `/feed` (16,500 against 2,650): without the process start a post request is
one small query and a 195-byte response. A feed is 20 posts, ~20 KB of JSON that TeX has to take apart and
rebuild character by character in macros, which costs several times more CPU than the rest of the request.
Likes are limited by the commit of a single Postgres (and they do not move with `WORKERS`).

This is a real number, not a cached one: at 18,000 `/posts/1` per second Postgres reported 18,400 commits per
second (one statement per request, autocommit), `ab` reported no failed requests and no length mismatches, and a
check under that load compared `/posts/<random id>` with the same row read straight from Postgres for 150 random ids
(0 mismatches), and a like followed by an immediate read showed the new count 100 times out of 100.

`WORKERS` at c=32 (feed / post / like, req/s): 5 gives 2,459 / 15,956 / 193, 10 gives 2,710 / 16,799 / 192, 20 gives
2,660 / 17,085 / 236. They are within noise of each other on one machine with a Postgres next to it; 2 x cores is
the default because on the real box the database is across a network and a worker that waits for a query is idle.

**Endurance.** One worker (`WORKERS=1`, recycling off), 30,000 mixed requests over one keep-alive connection
(feed, posts, health, authed likes, tampered / expired / wrong-algorithm / garbage tokens, `POST /posts` with
escapes, non-BMP and multi-byte text, extra JSON keys with a new name every time, malformed bodies, unknown routes):
no 500, no unexpected status, and the `pdftex` log at exit shows 29 strings, 2,107 multiletter control sequences and
71,336 words of memory, no "capacity exceeded". Process RSS grew about 0.6 to 0.9 KB per request (17 MB to 51 MB), which
is why workers are recycled (see "Recycling"). The test suite also passes with `WORKERS=1` and with
`WORKERS=1 WORKER_MAX_REQUESTS=3`.

**Idle connections** (`--ulimit nofile=1048576`, 20,000 keep-alive connections that each sent one `GET /health`
and then sit idle, then `ab -k -c 32 -t 8` on `GET /posts/1`):

| | v2 (`poll()`, `fork` per request) | v3 (`epoll`, workers) |
|---|---|---|
| `GET /posts/1`, c=32, no idle connections | 963 req/s (30 ms) | 16,657 req/s (3 ms) |
| same, 20,000 idle connections held | 337 req/s (117 ms) | 16,619 req/s (2 ms) |
| host RSS with 20,000 idle connections | 201 MB | 14.8 MB |
| time to open the 20,000 connections | 42 s | 1.4 s |

With `epoll` and a small per-connection struct, holding 20,000 idle connections is free; with `poll()` every loop
round scanned all of them.

A million users at one page per 24 s want about 42,000 req/s. v3 does 2,650 feeds per second here, about 16 times
fewer; at one feed (the heavy request) per user per 24 s that is on the order of 60,000 users on 10 laptop cores
if only feeds were requested, and the real mix has cheaper requests, so something between 50,000 and 100,000 users is a guess
that is consistent with these numbers, not a measurement. It is nowhere near a million. The 16-core number on the
real box should be somewhat higher and the 4-core number lower. The remaining cost is TeX itself taking a 20 KB
feed apart one token at a time, and that is the point of the submission.

The same setup also passes the test suite (`passed=42 failed=0`; pdfTeX 1.40.25, TeX Live 2023/Debian), as does
macOS with TeX Live 2026 (with the `poll()` backend).
