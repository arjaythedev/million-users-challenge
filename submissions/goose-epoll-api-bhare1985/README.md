# goose-epoll-api: an API server you describe in a file

| | |
|---|---|
| Language | [Goose](https://github.com/aardappel/goose), compiled by Goose's own compiler (commit `2ed1a63dde2ddff777fbb7f4790412b4f00cd47d`, built by `build.sh`) to C, then gcc |
| Framework | none: its own HTTP/1.1 server on an epoll loop per CPU, in this folder |
| Postgres driver | libpq 18.6, static, in pipeline mode with prepared statements |
| Compression | ISA-L 2.31.1 (igzip, level 0), static |
| Dependencies | exactly those two (SHA-256-pinned source tarballs, the same ones `c-epoll-unorig-username` builds) and the Goose compiler |

A fast Postgres-backed JSON API server whose **routes, SQL, request checks and response shapes are in an OpenAPI 3.1 file**, not in
the code. Change the file, restart, and it serves a different API, on the same event loop and the same Postgres pipelining. No
Goose is needed to do that, and the file is an ordinary OpenAPI document: editors, linters and Swagger UI read it (it passes
`redocly lint` with no warnings). What OpenAPI cannot say (the SQL, the response body built from a result) is in `x-` extensions.

The benchmark's own five endpoints are `openapi.json` in this folder. The server passes the 42 checks of `test/test.sh` with them.
`examples/` has two other APIs on the same tables, to show the file doing something the benchmark does not:
`blog.openapi.json` (statistics, one user's posts with a `limit` in the query string, an owner-checked delete that answers 409 when
the post has likes) and `profiles.openapi.json` (a profile with counts, a text search, editing your own post).

It is built on the design of `c-epoll-unorig-username`: one `SO_REUSEPORT` epoll loop per CPU, libpq in pipeline mode with
prepared statements, writes on their own connections and answered only after the commit, ISA-L igzip for large bodies, a short
batching pause so one wake-up serves many requests.

- The Goose compiler is built from a pinned commit by `build.sh` (cmake); the v2026.3 release predates `root struct` and `slice_pool`.
- Postgres connections: 64 in all (`max(2, 64 / threads)` per thread). Ports: `PORT` and `EXTRA_PORT`, no Nginx.
- Config (environment): `DATABASE_URL`, `JWT_SECRET`, `HOST`, `PORT`, `EXTRA_PORT`, as in SPEC.md. The API is the `openapi.json` beside
  `start.sh`; `API_CONFIG` can name another file, and the benchmark does not use it.

```bash
sudo ./install.sh      # once, as root: compiler, curl, git, cmake, nasm, bison, flex
./build.sh             # first run fetches and builds everything
DATABASE_URL=postgres://... JWT_SECRET=... PORT=3000 ./start.sh
API_CONFIG=examples/blog.openapi.json ./start.sh
```

`GOOSE=/path/to/goose ./build.sh` uses a compiler you already have (built from commit `2ed1a63d`).

## What an operation is

```json
"/posts/{id}/like": {
  "post": {
    "security": [ { "bearer": [] } ],
    "parameters": [ { "name": "id", "in": "path", "required": true,
                      "schema": { "type": "integer", "minimum": 1 }, "x-error": "invalid post id" } ],
    "x-sql": {
      "query": "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING",
      "params": ["jwt.uid", "path.id"],
      "write": true,
      "errors": { "23503": { "status": 404, "error": "post not found" } }
    },
    "responses": {
      "200": { "description": "Already liked.",
               "x-template": { "liked": true, "already_liked": "{{affected==0}}", "post_id": "{{path.id}}" } },
      "201": { "description": "Liked.", "x-when": "affected == 1" }
    }
  }
}
```

The reader takes the standard parts of OpenAPI (paths and their `{name}` segments, `parameters`, `requestBody`, `security`,
`responses`, `$ref` into `#/components/...`) and the `x-` extensions below, and ignores the rest (`summary`, `description`,
`info`, `servers`, unused schemas...), so the file can document the API as well as define it. JSON only, no YAML.

A request is handled in this order, and the first thing wrong is answered at once:

1. **Route.** The method and the path without its query string. No match: 404 `{"error":"not found"}`.
2. **Token** (a non-empty `security`): `Authorization: Bearer <jwt>`, HS256 with `JWT_SECRET`, `exp` required, `nbf` honored, nothing
   remembered between requests. Failures are 401: `missing bearer token`, `invalid or expired token`, `invalid token payload`.
3. **Values**: the token's claims, then the path, then the query string, then the body. One that is not valid is a 400 with the
   message the file gives for it.
4. **The statement** is sent with its parameters. One that writes (`"write": true`) gets its own transaction and its response goes
   out only after the commit. Reads are pipelined and packed.
5. **The response** is built from the result as the file says.

### Values

| Written as | Comes from | Declared by |
|---|---|---|
| `path.NAME` | the `{NAME}` segment of the path | a `parameters` entry with `"in": "path"` |
| `query.NAME` | `?NAME=...` | a `parameters` entry with `"in": "query"`, with a `default` in its schema or `"required": true` |
| `body.NAME` | a property of a JSON body | `requestBody.content["application/json"].schema.properties.NAME` |
| `jwt.NAME` | a claim of the token | `components.securitySchemes.*."x-claims": { "NAME": { "claim": CLAIM, "type": ... } }` |

A parameter's `schema` gives its type: `integer` with `minimum: 1` is an `id` (a positive integer), any other `integer` is an `int`,
anything else is text. A path or query value of type `id` or `int` that is not one is the 400 in the parameter's `x-error`; an `id` is
also normalized (`0007` is 7, and a number too big for 63 bits becomes the largest one, which finds nothing). A JWT claim of type
`id` is a string holding a positive integer of up to 15 digits (`"type"` in `x-claims` is `id`, `int` or `text`).

A body property is a JSON string. It may say `"x-trim": true` (JavaScript's `String.trim`, before the checks), be listed in
the schema's `required` (its `"x-required-error"` is the 400 for a missing or empty one; without `required` a missing property
is just empty), and have `"maxLength": N` with `"x-too-long-error"` as the 400 for a longer one (characters, not bytes). A body that is
not JSON, or a property of the wrong type, is the 400 in the request body's `"x-malformed"`.

### The statement

`x-sql.query` is any single statement. It is prepared on every Postgres connection at start-up, so a mistake in it stops the
server then, with Postgres's own message. `params` lists the values for `$1`, `$2`, ... in order. They go to Postgres as text and
Postgres infers their types from the SQL, so write it as you would for psql. The session is in UTC.

### The response

`responses` are keyed by status code:

- The first 2xx response without `x-when` is the normal one. Its **`x-template`** is the body, any JSON. A string of exactly the form
  `"{{...}}"` is replaced by a value:

  | | |
  |---|---|
  | `{{path.x}}`, `{{query.x}}`, `{{body.x}}`, `{{jwt.x}}` | a value of the request (a number for `id` and `int`, a JSON string for `text`) |
  | `{{rows}}` | every row, as an array of objects |
  | `{{row}}` | the first row, as an object (`null` if there is none) |
  | `{{row.column}}` | one column of the first row |
  | `{{affected}}` | the number of rows the statement changed |
  | `{{affected==N}}` | `true` or `false` |
  | `{{uptime_s}}` | seconds since the server started |

  A row's keys are its columns' names, and `x-sql.columns` (`{ "username": "author" }`) renames them (so the SQL can stay as the
  spec has it and the JSON still says `author`). Values are written by the column's type: integers, floats, numeric and json/jsonb as
  they are, booleans as `true`/`false`, timestamps in UTC with milliseconds (`2025-12-31T23:59:19.409Z`, truncated), NULL as `null`,
  anything else as an escaped string.
- **`"x-when": "affected == N"`** gives another status, with the same body, when the statement changed exactly that many rows
  (the `201` above, or a `404` for an update that matched nothing).
- **`"x-when": "no_rows"`** is the answer for a query that returns no rows: `{"error": <its "x-error", "not found" if left out>}`.
- **`"x-when": "db_error"`** (with an `x-template`) is the answer when no Postgres connection is usable at all.

`x-sql.errors` maps a Postgres SQLSTATE to `{ "status": ..., "error": ... }` for a statement Postgres refuses (`23503` foreign key,
`23505` unique), and anything else it refuses is a 500. Other keys (`default`, `4XX`) and `x-when`-less non-2xx responses only
document.

### Limits

At most 32 parameters per statement, 8 body properties and 8 JWT claims, 64 result columns. Bodies are JSON, results are read as
text, the token scheme is HS256 `Bearer`, and one statement runs per request. A non-empty `security` means the bearer token is
required whatever scheme it names. The only headers set are `Content-Type`, `Content-Length` and `Content-Encoding`. Statuses with a
text line are 200, 201, 202, 400, 401, 403, 404, 405, 409, 410, 422, 429, 500, 502, 503 and 504.

## How it is built

| File | What it holds |
|---|---|
| `server.goose` | the connection table, the Postgres pipeline, the OpenAPI reader, request handling, the response writer, the loop |
| `io.c`, `io.goose` | the epoll loop's sockets, behind a small event interface |
| `http.goose` | HTTP/1.1 request parsing and the status lines |
| `jsn.goose` | strict JSON validation and string decoding |
| `auth.goose`, `crypto.goose` | HS256 JWT checking, SHA-256 and HMAC |
| `wire.goose` | JSON string escaping and JavaScript's `String.trim` |
| `pq.goose`, `shim.c` | libpq, and the few calls that need C arrays or strings; ISA-L gzip |
| `openapi.json`, `examples/` | the benchmark's API, and two others |
| `tests/` | tests of the pieces that can be run on their own |

The OpenAPI file is read once, when the server starts, and compiled into flat tables (routes, values, parameters, response pieces).
That takes under 5 ms. A request then walks those tables and appends bytes; the document is not looked at again. The only things
matched by name at run time are a route's literal segments, the names in a query string, and a result column named in
`{{row.column}}`. The first version of this server read a private `api.json` format;
moving to OpenAPI changed the reader only. Instructions per request are identical (116.4k at 12,000 users, before and after), and so
are cycles within noise (108 to 110k).

## Measured here

AMD Ryzen 5 3550H (4 cores / 8 threads), Linux 7.0, Postgres 18 in Docker on the same machine. The load generator, the database and the
server share it, so treat the numbers as rough, and read them with this in mind: **this CPU's clock follows its load**
(`schedutil`, 1.4 to about 3.7 GHz), so CPU time per request is not comparable between runs. Every figure below is **cycles per
request** (`perf stat`, over a 30 s steady-state window), which is. Even so, runs of the same server differ by a few percent.
Everything is one server thread on CPU 0, with `mload` and gzip.

**Against a Postgres that answers at once.** A small program that speaks enough of the v3 wire protocol for libpq in pipeline
mode and answers each statement with pre-made rows of the real shapes and varying sizes (random text of 20 to 280 characters,
usernames, timestamps). It takes the database, and any CPU it would use, out of the measurement. The C entry and this server, side by
side, at four loads (`-think-scale 0.1`):

| users | req/s | | kcycles / request | p50 | p95 | p99 |
|---|---|---|---|---|---|---|
| 12,000 | 12,570 | C entry | 108.3 | 8.4 ms | 18.0 ms | 26.0 ms |
| | 12,560 | this | 108.1 | 8.6 ms | 18.1 ms | 25.4 ms |
| 24,000 | 25,010 | C entry | 113.3 | 8.1 ms | 34.3 ms | 61.1 ms |
| | 25,020 | this | 113.3 | 8.3 ms | 34.3 ms | 60.5 ms |
| 32,000 (one thread saturated) | 31,620 | C entry | 111.5 | 48 ms | 118 ms | 150 ms |
| | 31,430 | this | 113.6 | 54 ms | 129 ms | 165 ms |
| 38,000 (overloaded) | 33,150 | C entry | 111.3 | 170 ms | 283 ms | 316 ms |
| | 32,350 | this | 114.0 | 202 ms | 332 ms | 374 ms |

So below saturation the two are indistinguishable; at saturation this server uses about 2% more cycles per request and, overloaded,
serves 2.5% fewer requests per second. That is what reading the API from a file costs: results are read as text and the response is
built from a table, where the C entry reads binary and writes its five responses directly. The same server with the five endpoints
hard-wired in the code used 7 to 8% fewer cycles than this one.

**Against real Postgres**, 14,000 users (about 14,000 req/s, the per-thread rate of the best entries at their 4-core limit), a
5-minute hold:

| | req/s | p50 | p95 | p99 | max | kcycles / request |
|---|---|---|---|---|---|---|
| C entry | 14,015 | 7.3 ms | 18.9 ms | 31.4 ms | 75 ms | 105.9 |
| this | 14,003 | 7.6 ms | 19.8 ms | 32.6 ms | 100 ms | 110.5 |

No errors in any run. Both passed with a wide margin (p95 under 500 ms, p99 under 1 s).

**Batching window.** After an iteration with fewer than 32 events (`BATCH_MIN`) the loop sleeps 2 ms (`BATCH_US`) before waiting for the
next, as the C entry does. One wake-up then serves many requests and one Postgres `Sync` covers many reads. Against a Postgres that
costs nothing, this cut the cycles per request by 20% at 12,000 users (136.0k to 108.1k) and 8% at 24,000, with context switches
per request falling from 0.31 to 0.02 and TCP segments from 8.3 to 6.5; at saturation it changes nothing (a busy thread does not
sleep). It costs latency: p95 of 2 ms becomes 18 ms. A longer window (4 ms and 96 events) saved nothing more and tripled p95, so
it was dropped.

**Tests.** `test/test.sh` passes 42/42. A differential run of 80 probe sequences (feed with and without gzip, escapes, surrogate
pairs, oversize and malformed bodies, bad tokens, `HTTP/1.0`, `Expect: 100-continue`, pipelined and split requests) gives
responses identical (once timestamps and new ids are masked) to those of the earlier build with the five endpoints hard-wired, which
had matched the C entry's byte for byte; this was run again after the move to OpenAPI.
A connect/close churn test (64 long-lived connections that must never fail while 48 threads connect and close as fast as they can,
politely and with resets, with requests in flight, with half requests) passes: about 83,000 requests with no failure, 240,000
connections churned, nothing left established. An OpenAPI file with a SQL statement Postgres refuses, a placeholder that is not
one of the operation's values, an `x-when` it cannot read, a `{name}` in the path that is not declared, a query parameter with no
default, a missing 2xx response, a document that is not JSON or not OpenAPI 3, or no operations at all, stops the server at
start-up with a message (14 such files were tried). The two examples were exercised with curl on a copy of the seed
database: success, 400, 401, 404 and 409 paths, an owner's edit and delete, and a delete refused by the foreign key.

## How it got here, and where it stopped

It started as a straight port of `c-epoll-unorig-username` into Goose, same design. That part went well: 42/42 and about the same
CPU per request as the C entry. What followed was a long hunt for the last cycles, and what it found decided what this submission is.

- **Profiling the user-space code.** `perf` showed Goose's standard `find` and `each_split` scanning for `\r\n\r\n` byte by byte, and
  the timestamp formatting, as the places the port spent more than the C entry. Fixing those (a one-byte vector search per header
  line, digit pairs from a table) took about 7% off the user-space cycles.
- **The measuring stick moved.** This machine's clock follows its load, so CPU time per request is not comparable between runs. One
  test made io_uring look 44% cheaper than epoll; the work per request was the same (68.2k against 71.4k cycles) at different
  clock speeds. From then on everything was cycles and instructions per request, and the earlier numbers were discarded.
- **io_uring.** A complete io_uring engine (direct descriptors, multishot accept and receive, provided buffers) and an epoll engine
  behind one interface, both tested against real Postgres and against a Postgres that answers instantly. At saturation plain
  io_uring was about 2% ahead in requests per second and 4% in cycles; a registered ring fd changed nothing; `SQPOLL` cost 60%
  more cycles per request, and `SQPOLL` with spin-waiting more than double. That is not enough to pay for a second engine, twice
  the resident memory (25 against 13 MB at 200,000 connections) and a dependence on kernel features I cannot test on the
  benchmark box (the leaderboard records an io_uring entry that failed on its kernel). Epoll only.
- **libdeflate** (the goose-libdeflate package) made the feed 15% smaller but cost 21% more cycles than ISA-L level 0. Not used.
- **The batching pause** is the C entry's (2 ms after fewer than 32 events). Tested against no pause at all, it saves 20% of the cycles
  at moderate load and nothing at saturation. A longer one (4 ms, 96 events) saved nothing more and tripled p95; I had kept it for a
  3% gain that did not survive a like-for-like test, and went back to the C entry's.
- **Reading the API from a file** cost 7 to 8% more cycles per request than the same server with the five endpoints hard-wired.
- **The file format.** The first version read a small format of its own (`api.json`). It was replaced by OpenAPI 3.1 with `x-`
  extensions, so the file is a document other tools understand and the reader is the only thing that changed: the tables it fills
  and the request path are the same (identical instructions per request, 80 differential sequences identical).

At that point every remaining gain was a few percent, bought with hand-tuned user-space code, a second engine, or kernel features. I
stopped chasing them. The aim became a submission that is **safe and understandable rather than squeezed**:

- **Real, known C libraries where it matters**: libpq and ISA-L, the same two the C entry builds from the same pinned sources, plus
  about 270 lines of C of our own (`io.c`, `shim.c`) that wrap sockets and the few libpq calls that need C arrays.
- **Written in Goose where it is ours.** Goose checks slices and arrays, so a class of memory errors cannot occur in the roughly
  2,000 lines of HTTP, JSON, JWT and routing logic, and the server stays small (8 MB resident after 240,000 connections churned;
  13 MB at 200,000 held). That is less of a memory-safety concern, not none: libpq, ISA-L and our two C files are C, and a crash is
  not restarted on the benchmark box. The tests below ran clean; they are evidence, not proof.
- **Flexible.** Because the API is a file, the same server can be pointed at other SQL and other routes, for prototypes, internal
  tools and experiments, without touching code.

The result is parity with the C entry: the same below saturation, about 2% more cycles per request at saturation and 2.5% fewer
requests per second when overloaded. Not a win over it.

Tests run on this build: `test/test.sh` 42/42; a differential run of 80 probe sequences against the earlier hard-wired build; a
connect/close churn test (64 long-lived connections that must never fail while 48 threads connect and close as fast as they can,
politely and with resets); OpenAPI mistakes (a SQL statement Postgres refuses, a bad placeholder, and 12 more) stopping the
server at start-up; both examples exercised with curl; 5-minute holds on real Postgres; and, on earlier builds of the same server, the official load
profile holding 200,000 users on 4 CPUs (280,000 failed for every server tried, because the load generator ran out of memory).

## What this is, and what it is not

It is built for the benchmark and for prototypes. It is **not hardened for production**: no TLS (the benchmark box serves plain
HTTP), HS256 tokens only, no rate limiting, one statement per request. The OpenAPI file is **trusted input**: the SQL in it runs with
whatever rights `DATABASE_URL` has, so it is as powerful as code and should be reviewed like code. The `openapi.json` shipped here runs
exactly the queries in SPEC.md; a different file that did more of the work inside Postgres would not be the benchmark's API (rule 3).

## Open work

- A request carried over between reads is capped at 132 KB (the C entry allows 256 KB); the in-flight ring per Postgres
  connection is 1,024 statements and a full ring answers 500.
- Header lines are split on CRLF; a bare CR inside a header value is not treated as a line end (the C parser does).
- The JSON validator is checked by `test/test.sh` and a differential run, not fuzzed.
- `install.sh` and `build.sh` have not been run from scratch on a clean Ubuntu 24.04 box.
