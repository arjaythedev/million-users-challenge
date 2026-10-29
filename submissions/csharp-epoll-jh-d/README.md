# csharp-epoll-jh-d: C# on raw epoll, one loop per core, its own pipelined Postgres client

The feed API (`SPEC.md`) in C# with no web framework and no NuGet packages. It runs one epoll loop per core on raw
Linux file descriptors, with a hand-written HTTP/1.1 parser and a hand-written Postgres wire-protocol client that
lives inside those same loops. It passes `test/run.sh` (42/42) on `PORT` and, with the ports swapped, on `EXTRA_PORT`.

The idea: at a million users the request rate is modest (~42k req/s). The hard parts are a million idle keep-alive
connections in 32 GB, and CPU per request once the kernel's network work shares the same cores. So:

- An idle connection is a 40-byte struct in native memory, with no buffer and no managed object, so the GC heap
  stays the same size whether 10 or 400,000 clients are connected.
- A request never leaves the thread that accepted it. The HTTP parse, the JWT check, the Postgres round trip,
  decoding the rows into JSON, gzip and the send all run on one loop thread: no thread pool, no locks, no hand-offs.

## Stack

| Piece | Version |
|---|---|
| Runtime | .NET 10: SDK 10.0.403 (pinned in `install.sh` and `global.json`), runtime 10.0.14, framework-dependent, JIT with tiered PGO |
| HTTP | none: epoll and sockets through `[LibraryImport("libc.so.6")]`, own HTTP/1.1 parser |
| Postgres | own client for protocol v3: SCRAM-SHA-256 / MD5 / cleartext auth, prepared statements, binary results, pipelining |
| JSON | own writer for responses; `System.Text.Json` (`Utf8JsonReader`, `JsonDocument`) for JWTs and `POST /posts` bodies |
| JWT | HS256 with `IncrementalHash` (HMAC-SHA256) and `CryptographicOperations.FixedTimeEquals` |
| gzip | libdeflate 1.19 (`libdeflate0` from apt), level 1, via P/Invoke; zlib-ng (`System.IO.Compression`) if it's missing |

## Design

- **One epoll loop per core**, on a dedicated thread (`Environment.ProcessorCount`, which follows the affinity
  mask). Each loop has its own `SO_REUSEPORT` listeners on `PORT` and `EXTRA_PORT`, its own client connections and
  its own Postgres connections, all in one epoll set. Nothing on the request path is shared between loops.
- **Raw file descriptors, no `Socket` objects.** `accept4`, `recv`, `send`, `epoll_ctl` and `close` are direct libc
  calls; the short non-blocking ones are `[SuppressGCTransition]`.
- **A client connection is a 40-byte struct** in one native table indexed by fd. Each loop receives into one 64 KB
  buffer. A connection gets its own heap block only while it holds a partial or pipelined request, or a response
  the socket didn't take in one `send`.
- **HTTP:** requests are parsed in place. Only `Authorization`, `Content-Length`, `Accept-Encoding`, `Connection`,
  `Expect` and `Transfer-Encoding` are looked at. Keep-alive, HTTP/1.0, pipelining (answered in order) and
  `Expect: 100-continue` all work. Chunked request bodies get a 400 (out of scope).

### Postgres, inside the loops

- **Connecting** is the only blocking part, and it runs on a background thread: TCP connect, startup, authentication
  (SCRAM-SHA-256 with the server signature checked; MD5 and cleartext also supported), then one `Parse` per SPEC.md
  statement with fixed parameter types (`int8`, `text`). The ready socket is switched to non-blocking and handed to
  its loop through a queue plus an `eventfd` wake-up.
- **Requests are dealt out once per loop pass.** During the pass, requests that need Postgres are only collected
  (reads and writes apart). At its end, the reads are cut into ceil(n / `DB_DEAL`) contiguous runs of near-equal
  length (`DB_DEAL` = 16), and each run goes to one of the least-loaded read connections, as `Bind` (binary
  parameters, binary results) + `Execute` per request and one `Sync` and one `send()` per connection; the same for
  writes. Knowing the whole pass before choosing means a busy pass becomes a few large, evenly sized batches: fewer
  syscalls on both sides, fewer wake-ups of the backends and fewer implicit transactions (for writes, fewer commits).
- **Results are decoded in binary, straight into the response.** `int8`/`int4` are read big-endian, and
  `timestamptz` (µs since 2000-01-01) is formatted as UTC truncated to milliseconds. Text columns are copied as
  UTF-8 bytes, JSON-escaped, so no strings are allocated for the feed.
- **Reads and writes use separate connections** (`DB_READ_POOL`, default 64; `DB_WRITE_POOL`, default 16, in total,
  split evenly over the loops), so a read never waits behind a commit. The totals don't scale with this box's
  cores, because the database box has 16 cores either way.
- **Postgres runs each batch (one `Sync`) as one implicit transaction:**
  - **Reads** are answered as soon as their `CommandComplete` arrives.
  - **Writes are group-committed.** Their responses wait for the batch's `ReadyForQuery`, which comes after its
    commit, so no response leaves before its write is durable.
  - **After an error**, Postgres skips the rest of the batch. The failed request gets its error (a like on a missing
    post is SQLSTATE 23503 → 404). Writes that finished in the same batch were rolled back with it, so they are
    submitted again, as are the skipped requests.
- **A lost connection:** writes whose fate is unknown get a 500. Reads, and writes that were never sent, are
  submitted again on another connection. Reconnecting starts at once, then retries every second. An error whose SQLSTATE
  is in class 08 (connection exception) or 57P (shutdown, terminated backend) counts as a lost connection, not as
  the error of the request at the head. The log
  line says why: closed by Postgres, the `recv`/`send` errno, the socket error, or the protocol message that made
  no sense.
- **An error at commit** (after every request of the batch has its answer) rolls the batch back: its writes are
  submitted again, and the next batch's answers stay matched to their requests.
- **A silent connection:** one that owes answers and has sent nothing for `PG_TIMEOUT_MS` (10 s) is dropped and
  handled as lost. That also covers a database box that vanished without closing anything: the next batch sent to
  it gets no answer.
- **No live connection:** requests wait up to 5 s for one, much like a pool waits for a free connection, and get
  a 500 after that. `/health` answers at once: 503 `degraded`, with the last connection error.

### Responses and timing

- **Responses:** compact JSON with `Content-Length` (never chunked) and a `Date` header rebuilt when the second changes. The
  JSON is written into a pooled buffer that leaves 256 bytes in front for the HTTP head, written in place once the
  length is known. Feeds (~5 KB) are gzipped to ~1.9 KB when the client accepts gzip; bodies under 1 KB are sent as
  they are.
- **Batching window (`BATCH_US`, default 2000):** a pass of the loop starts at most every 2 ms: after a pass, the
  loop sleeps for what is left of the window. More requests and Postgres results then pile up for the next pass
  and share its syscalls and `Sync`s, and the batches grow with the load instead of shrinking. A pass that took
  the whole window, or that couldn't fetch every ready event, doesn't sleep.
- **No idle timeout and no TCP keep-alive probes from the server side.** The listener sets `TCP_NODELAY`, which
  accepted sockets inherit.
- **An exception while handling one connection** is logged and closes that connection (or that Postgres
  connection). It never takes the process down. So are failed `accept4`/`epoll_ctl` calls on new connections.
  A bug in a client's pipelined request, run when an answer for it arrives, closes that client, not the Postgres
  connection that delivered the answer. Anything else is caught around the whole pass, which is logged and the
  loop carries on.
- **Closed client fds are closed at the end of the pass,** so another loop can't be handed the same fd number
  while this pass still holds an event for it.
- **A stats line per loop every `STATS_SEC` (10 s):** open connections, accepts and closes per second, Postgres
  answers per second, loop passes per second, requests per `Sync`, queued Postgres requests (total and the longest
  queue), requests waiting for a connection, dropped Postgres connections.

## Rules

- **Rule 3:** each request runs one SPEC.md statement, verbatim, prepared once per connection. Only pipelining and
  batching are used: several reads in one implicit transaction, and several writes committed together. No SQL-built
  JSON, no extra queries.
- **Rule 7:** no response, query-result or token cache, and nothing about a token is remembered. Kept between
  requests: connections, prepared statements, each loop's HMAC instance (key pads only), and the current second's
  `Date` header.
- **Rule 8:** a write's response is sent only after the `ReadyForQuery` that ends its batch, which is after the
  commit. `synchronous_commit` is untouched.
- **Rule 9:** `alg`, signature, `exp` (and `nbf` if present), `sub` and `username` are checked on every
  authenticated request.
- **Rule 2:** no settings are changed, in the session or server-wide.
- **Rules 4, 5:** nothing is created or altered in the database. 80 Postgres connections by default; the server
  refuses to start with more than 500.
- **Rule 13:** only in-process settings: Server GC (non-concurrent), tiered PGO, invariant globalization. The soft
  `RLIMIT_NOFILE` is raised to the hard limit at startup.

## Settings

| Env | Default | |
|---|---|---|
| `LOOPS` | cores | Event loops |
| `DB_READ_POOL` / `DB_WRITE_POOL` | 64 / 16 | Postgres connections in total, split evenly over the loops (rounded up) |
| `DB_DEAL` | 16 | Target requests per Postgres round trip when a pass's requests are dealt out |
| `PG_TIMEOUT_MS` | 10000 | A Postgres connection owing answers and silent this long is dropped |
| `BATCH_US` | 2000 | Batching window, above |
| `STATS_SEC` | 10 | Stats line per loop (0: off) |
| `GZIP_LEVEL` | 1 | libdeflate level |

## Scripts

- `install.sh` (root, idempotent): installs `libdeflate0` if missing, and .NET SDK 10.0.403 into `/usr/share/dotnet`
  with Microsoft's `dotnet-install.sh` unless that exact version is already there.
- `build.sh`: a Release `dotnet publish` into `out/`. There are no package dependencies.
- `start.sh`: runs `out/FeedServer` (the published app host) in the foreground.

## Files

| File | |
|---|---|
| `src/Program.cs` | Config, startup |
| `src/EventLoop.cs` | Epoll loop, client connection table, receive/send, routing requests to Postgres connections |
| `src/Pg.cs` | Postgres client: handshake and auth, Bind/Execute/Sync encoding, result parsing, batches and retries |
| `src/Api.cs` | Routes, validation, turning each statement's results into the response |
| `src/Http.cs` | Request parser, response heads |
| `src/Jwt.cs` | HS256 verification |
| `src/Json.cs` | JSON writer |
| `src/Gzip.cs` | libdeflate / zlib-ng |
| `src/Db.cs` | SPEC.md's SQL, rate-limited error log |
| `src/Native.cs` | libc imports |
