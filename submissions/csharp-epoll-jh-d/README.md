# csharp-epoll-jh-d: C# on raw epoll, one loop per core, its own pipelined Postgres client

The feed API (`SPEC.md`) in C#, with no web framework and no NuGet packages. It runs one epoll loop per core on raw
Linux file descriptors, with a hand-written HTTP/1.1 parser and a hand-written Postgres wire-protocol client that
lives inside those same loops.

The idea: at a million users the request rate is modest (~42k req/s). The hard parts are a million idle keep-alive
connections in 32 GB, and CPU per request once the kernel's network work shares the same cores. So an idle
connection costs as little memory as possible, and a request costs as few syscalls and hand-offs as possible.

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

- **One epoll loop per core**, each on its own thread (`Environment.ProcessorCount`, which follows the affinity
  mask). Each loop has its own `SO_REUSEPORT` listeners on `PORT` and `EXTRA_PORT`, client connections and
  Postgres connections, all in one epoll set. A request never leaves the thread that accepted it: HTTP parse, JWT
  check, Postgres round trip, JSON, gzip and send all run there. There's no thread pool and no locks.
- **An idle connection is a 40-byte struct** in a native table indexed by fd, with no buffer and no managed object,
  so the GC heap stays the same size however many clients are connected. The table is sized to the file limit, and
  its pages are only committed when used. A connection gets a heap block only while it holds a partial or
  pipelined request, or a response the socket didn't take in one `send`. There's no idle timeout.
- **Raw file descriptors, no `Socket` objects:** `accept4`, `recv`, `send`, `epoll_ctl` and `close` are direct libc
  calls. The short non-blocking ones are `[SuppressGCTransition]`.
- **HTTP** requests are parsed in place. Keep-alive, HTTP/1.0, pipelining (answered in order) and
  `Expect: 100-continue` all work.
- **Batching window:** a loop pass starts at most every 2 ms, so requests and Postgres results pile up and share the
  pass's syscalls and `Sync`s, and batches grow with the load. A pass that took the whole window, or couldn't fetch
  every ready event, doesn't sleep.
- **Requests are dealt out to Postgres once per pass.** During a pass, requests that need Postgres are only
  collected, reads and writes apart. At its end, each kind is cut into contiguous runs of about 16, and each run
  goes to one of the least-loaded connections: `Bind` + `Execute` per request, then one `Sync` and one `send()` per
  connection. Under load, a pass becomes a few large, even batches, with fewer syscalls and backend wake-ups on
  both sides.
- **Reads and writes use separate connections** (64 and 16 in total, split over the loops), so a read never waits
  behind a commit. The totals don't depend on this box's cores, because the database box has 16 either way.
- **Results are decoded in binary, straight into the response JSON**, with no strings allocated for the feed. Feeds
  (~5 KB) are gzipped to ~1.9 KB; bodies under 1 KB are sent as they are.
- **Writes are group-committed.** Postgres runs each batch (one `Sync`) as one implicit transaction. A write's
  response waits for the batch's `ReadyForQuery`, which comes after the commit.
- **Errors:**
  - **A failed statement:** Postgres skips the rest of its batch. The failed request gets its error (a like on a
    missing post is SQLSTATE 23503 → 404). The batch's other writes were rolled back with it, so they are
    submitted again, along with the skipped requests.
  - **A lost connection:** writes whose fate is unknown get a 500. Reads, and writes that were never sent, go to
    another connection.
    - SQLSTATE classes 08 and 57P count as a lost connection.
    - A connection that owes answers and stays silent for 10 s is dropped.
    - Reconnecting retries every second.
  - **No live connection:** requests wait up to 5 s for one, then get a 500. `/health` answers 503 meanwhile.
  - **An exception** while handling one connection closes that connection, never the process.
- **A stats line per loop every 10 s** in the log: connections, Postgres answers per second, requests per `Sync`,
  queued and dropped Postgres requests.

## Rules

- **Rule 3:** each request runs one SPEC.md statement, verbatim, prepared once per connection. No SQL-built JSON
  and no extra queries.
- **Rules 2, 4, 5:** nothing is changed in the session, in the server's settings or in the schema. The server uses 80
  Postgres connections on 16, 8 or 4 cores.
- **Rule 7:** nothing is cached and nothing about a token is remembered. Kept between requests: connections,
  prepared statements, each loop's HMAC instance (key pads only), and the current second's `Date` header.
- **Rule 8:** a write's response leaves only after its commit. `synchronous_commit` is untouched.
- **Rule 9:** `alg`, signature, `exp` (required), `nbf` (if present), `sub` and `username` are checked on every
  authenticated request.
- **Rule 13:** only in-process settings: Server GC (non-concurrent), tiered PGO and invariant globalization. The soft
  `RLIMIT_NOFILE` is raised to the hard limit at startup.
