# Erlang/OTP on raw `gen_tcp`: one process per connection

Erlang is a good fit for this challenge for one reason: a connection is a process, a process is
about 2.6 KB, and a process blocked in `gen_tcp:recv/3` costs no CPU. The hard part is the other
half of the problem: at one million users the load generator still sends about 42,000 requests per
second, and every one of them has to parse HTTP, run one Postgres query, build JSON, optionally
gzip it, and get out of the way.

This is a small HTTP/1.1 server written directly on `gen_tcp`, with `epgsql` for Postgres
and `jsone` only for JWT/request JSON. There is no Cowboy, no Nginx, and no pool checkout in the
request path.

| | |
|---|---|
| Language / runtime | Erlang/OTP **25.3.2.8** on Ubuntu 24.04 (`install.sh` installs `erlang-nox`; the local numbers below used OTP 27 / ERTS 15.2.7.4, and the code avoids OTP-27-only APIs) |
| HTTP | none: raw `gen_tcp`, hand-written HTTP/1.1 parser, keep-alive, pipelining, `Expect: 100-continue` |
| Postgres driver | `epgsql` **4.8.0** (pinned in `rebar.lock`) |
| JSON | `jsone` **1.9.0** for incoming JWT/body; responses are written as iolists |
| Build | `rebar3 compile` (rebar3 3.24.0 pinned in `install.sh`) |
| Ports | one listener on `PORT` and one on `EXTRA_PORT`, same process |
| DB connections | 128 by default (`DB_POOL`, 1..512); prepared statements, no checkout lock |

## Shape of the server

- **One process per accepted socket.** Four acceptor processes per listener call
  `gen_tcp:accept/1`; each connection is handed to a fresh process with
  `gen_tcp:controlling_process/2`. Accepted sockets inherit
  `{packet, raw}, binary, {active, false}, {nodelay, true}, {send_timeout, 30000}` and a
  `65535` listen backlog. Idle keep-alives time out after 120 s, comfortably above the
  spec's 75 s and the load generator's ~40 s maximum idle.
- **The request parser reads the request, not one header per syscall.** The listener is in
  `{packet, raw}` mode. The first `gen_tcp:recv(Socket, 0, 120000)` normally returns the
  request line and all headers in one binary (the load generator writes each request in one
  call); the process finds `\r\n\r\n`, parses the request line and only the headers it needs,
  and reads the `Content-Length` body (possibly arriving later) with one exact `recv`. Extra
  bytes are kept as the buffer for the next request, so HTTP pipelining and split bodies work.
  The previous implementation used the inet driver's `{packet, http_bin}` mode, which needs
  one `recv`/driver call for the request line and one for every header.
- **The same connection process handles the whole request.** It does the parse, the query,
  the JSON and the send; there is no worker hop. A full `erlang:garbage_collect()` after each
  response returns the process heap to its 233-word minimum, which is what makes one million
  mostly-idle processes affordable: after a request the process is back to the size of a
  sleeping Erlang process.
- **Responses are iolists.** Status line, header lines and ESC-free JSON pieces are constants
  or `integer_to_binary/1` results; `gen_tcp:send/2` writes the iolist. `Content-Length` is
  computed from the final iolist, including the gzip case.
- **Gzip only where it pays.** Bodies over 1024 bytes and only when the client advertises
  `gzip`; the feed is ~5.1 KB and compresses to ~2.0 KB, a get-post (195 B) and a 500-char
  create response stay plain. Compression is level 1 with a 4 KB-hash/15-bit window and the
  gzip wrapper (`deflateInit(Z, 1, deflated, 31, 4, default)`), which is about 15-25 % faster
  than `zlib:gzip/1` on the feed with a 5 % larger body, and its zlib state is about 7x
  smaller. A fallback to `zlib:gzip/1` keeps the response correct if a future zlib rejects the
  tuning.

## Database path

- **128 connections, four prepared statements each.** `DB_POOL` defaults to 128 (1..512),
  well under the challenge's 512-connection limit. Each connection parses
  the four SQL statements from `SPEC.md` with names `feed`, `get_post`, `insert_post` and
  `insert_like`; the `#statement{}` records are identical for every connection and are stored
  once in `persistent_term`. `feed_db:query/2` reads them without copying.
- **No central checkout process.** A `gen_server` only establishes/replaces connections and
  publishes the current tuple of live connection pids in `persistent_term`. A request picks
  the next connection with an `atomics` round-robin counter and calls
  `epgsql:prepared_query/3` directly. `epgsql` itself serialises commands per socket, so a
  busy connection queues only its own 1/128th of the traffic. The old design sent every query
  through one pool `gen_server` with a monitor, a checkout call and a checkin call; at 15 k
  req/s that path alone cost ~44 µs per request.
- **No retries on writes.** Reads (`feed`, `get_post`) retry once on a dead connection, since
  they are idempotent. `insert_post` and `insert_like` are never replayed; a lost connection
  returns 500 rather than risking a duplicate. SQL errors are returned as-is (`23503` is the
  like-on-missing-post 404).
- **Writes are committed before the response.** `prepared_query/3` waits for
  `ReadyForQuery`; nothing sets `synchronous_commit = off`.

### The transaction bug worth knowing about

`epgsql:parse/4` sends `Parse` + `Describe` + `Flush`, but **no `Sync`**. In the extended query
protocol an implicit transaction begins at the first `Parse` and ends at `Sync`, so preparing
the four statements at startup left every connection *idle in an open transaction*. The first
`INSERT` on that connection then used `now()` from connection setup, not from the request.
`pg_stat_activity` showed all 256 connections (the pool size used for that experiment) `active`
with `xact_start` at startup and the last query text `INSERT INTO likes ...`; two posts created
in quick succession on different connections could therefore get out-of-order `created_at`
values, and the second post could sort below the first in `/feed`.

The fix is one line in `feed_db:connect/1`: call `epgsql:sync/1` after preparing, before the
connection is published. All connections are then idle (`state = idle`, no `xact_start`),
and every row gets the actual statement time. This is also why the old versions could pass
`test/test.sh` accidentally: the test only requires `created_at` to be within 60 s of now, and
CI starts the server only seconds before the test.

## Other hot-path choices

- **Timestamp formatting is in the row codec.** `feed_pg_codec` decodes Postgres'
  `timestamptz` int64 directly into `YYYY-MM-DDTHH:MM:SS.mmmZ`, using precomputed two/three-digit
  and date tables in `persistent_term`; milliseconds are truncated (`div 1000`), as the spec
  requires. No `calendar`/`io_lib` call is made per row.
- **JWT verification has a header fast path.** The seed tokens all carry the same HS256 header
  (base64url `eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9`), so the fast path can skip decoding and
  parsing the header, but it still computes and compares the HMAC-SHA256 and checks `exp` on
  every request. Any other header goes through the full base64/JSON/alg check; `alg=none` is
  rejected. There is no verification cache.
- **JSON is assembled from constants and binaries.** `feed_json` takes the compiled
  escape-pattern once per feed and hoists it out of the row loop; the 20-row feed is an iolist
  with no intermediate JSON tree. Only request bodies and JWTs use `jsone`.
- **Headers are normalised without Unicode work.** Known header atoms map straight to the
  lowercase binaries the router uses (`authorization`, `content-length`, ...); only unknown
  names take the ASCII-lowercasing fallback. The one `{packet, http_bin}` recv per header is
  gone.

## Tuning considered

- **The `socket` NIF backend is not used.** OTP's own socket benchmarks show it is faster for
  bulk streams and connection churn, but still trails `inet_drv` on small request/response
  traffic, which is exactly this workload. `inet_drv` is also the path used by WhatsApp and
  every production Erlang HTTP server.
- **`{buffer, ...}`/`{recbuf, ...}` are left at their defaults.** The `inet` docs recommend
  keeping the user-level buffer at least as large as `recbuf`; setting one in isolation either
  adds copies or memory. The passive raw receive path does not keep an active-mode per-socket
  read buffer, and the 100 k measurement shows the default is already about 11.7 KB of RSS per
  connection including the process, port and driver state.
- **`TCP_DEFER_ACCEPT` is not set.** `mload` connects and then sleeps up to ~12.5 s before its
  first request at the scoring think time. Deferring accept would park those connections in the
  65,535-entry listen queue during a million-connection ramp.
- **Separate read and write Postgres connections were considered.** With 128 shared connections
  and ~2-3 k writes/s, a write occupies one connection for its commit (~0.3 ms), so a read meets
  a committing connection only about 0.5 % of the time. The split buys little at this pool size;
  the 1 M entries use far fewer connections per core, which is why they separate them.
- **Cross-request Postgres batching/pipelining is not used.** `epgsql:execute_batch/2` can send
  several Bind/Execute pairs in one round trip, but it runs them as one implicit transaction
  with one Sync; using it across requests would add a batching delay and make like/post error
  isolation harder. The 128 synchronous connections already keep the 0.25 ms DB round trip at
  low utilization.
- **A custom Postgres wire client was rejected.** It would remove the `epgsql` process hop and
  allow true per-query pipelining (the approach in the 1 M Java/Rust entries), but it is a
  SCRAM/protocol implementation that is much easier to get subtly wrong than to get faster
  without the benchmark boxes.
- **No allocator `+M` flags.** OTP's per-scheduler allocators are already in use, and the
  `erts_alloc` documentation warns that these flags need target measurements; the explicit GC
  already returns an idle connection to a 233-word heap.

## VM flags

`start.sh` runs the emulator with:

```
+P 4000000  +Q 4000000  +A 16  +IOt 2  +S "${nproc}:${nproc}"
+sbwt none  +sbwtdcpu none  +sbwtdio none
```

and exports `ERL_CRASH_DUMP_SECONDS=0`.

`+P`/`+Q` are set to 4,000,000 processes/ports: the current scoring caps the 16-core result at
1,000,000 users and searches 8/4-core runs up to 3,000,000. `+S` uses the box's vCPUs; `+A 16`
is enough for the file I/O the server actually does (async threads were 64 before, which was
wasted). `+IOt 2` gives socket event delivery two poll threads: one poll thread measured about
0.2 of a core at 15 k req/s, which is fine for the 1 M target but would be over a core at the
3 M search rate. `+sbwt none` stops the schedulers from busy-waiting between events; a server
with no client connections measured **0 CPU ticks over 15 s** with this setting. The crash dump
is disabled because a 1 GB+ dump on the benchmark box would only delay the inevitable.

## What the local runs showed

All local runs were on one 16-vCPU / 15 GiB WSL2 guest (AMD Ryzen 7 8700G) with Postgres and
the load generator on the same guest. That is harder than the challenge setup
(separate app box, separate DB box, load generators on other machines), and it means the
absolute CPU/latency numbers are pessimistic; the memory numbers are the useful part.

- **Official suite:** `bash test/run.sh submissions/erlang-gen_tcp-quefep-bmgo` on a fresh
  `feed_seed` copy: **passed=42 failed=0**.
- **10,000 users, real think time** (`mload -users 10000 -ramp 60s -hold 90s -gzip
  -think-scale 2.5`): 55,860 requests, 372 req/s, **p95 1.80 ms, p99 2.61 ms, 0 errors**;
  the server used 32 s CPU over the 150 s run, with Postgres and the load generator sharing the
  same 16 vCPUs.
- **100,000 users, connection/memory checkpoint** (`-users 100000 -ramp 60s -hold 90s -gzip
  -think-scale 5`, four loopback addresses x two ports): **100,000 established, 0 failed
  requests**, 296,842 requests. Server RSS went from 135 MB at startup to **1.24 GiB peak**,
  i.e. **11.7 KB per connection**; the last steady segment (t=115..150 s) had p95 1.3-2.2 ms.
  The whole-run p95 was 1.0 s because the connection storm and first pages all landed on the
  same saturated 16 vCPUs as `mload` and Postgres; the official 1 M ramp is spread over minutes
  on dedicated machines. The server stayed alive.

The 100 k RSS slope projects to **~11.1 GiB of BEAM memory for 1 M connections**, before
kernel socket memory. The challenge box has 32 GiB; `tcp_rmem`/`tcp_wmem` on it have a 16 KiB
default with a 4 MiB ceiling, and idle keep-alives do not hold 16 KiB each. At 1 M there is
room, so memory is not the risk. The risk is 42 k req/s of app + kernel CPU on 16 cores. The
local microbenchmarks put the pure-Epgsql query CPU at 24-38 µs and JSON feed encoding at
25 µs, so the architecture has a chance, but only the official two-box benchmark can answer it.
At the measured slope 3 M connections would need ~35 GiB before kernel memory, so the 1 M
16-core cap is the realistic target for this entry; the 8/4-core runs can go as far as the
box's memory allows.

I did not run 1 M locally: the official `mload` uses ~23 KB of RSS per simulated user
(2.34 GB measured at 100 k), so a 1 M-user generator needs ~23 GB before the server's ~12 GB;
this WSL guest has 15 GiB in total. A synthetic idle-socket client would fit after raising the
guest's memory, but it would test socket accounting, not the 42 k req/s workload. The 100 k run
is the largest run that fits here; the 1 M verdict needs the benchmark machines.

## Running locally

```bash
sudo bash install.sh                 # Erlang + rebar3 (once, as root)
bash build.sh                        # rebar3 compile
DATABASE_URL=postgres://feed:feed@127.0.0.1:5432/feed \
JWT_SECRET=million-users-challenge HOST=0.0.0.0 PORT=3000 EXTRA_PORT=3001 \
DB_POOL=128 bash start.sh            # foreground
```

`test/test.sh` (or `test/run.sh`, which creates a fresh DB copy) is the source of truth for the
API. The relevant background is in the Erlang Efficiency Guide (process memory and garbage
collection), the `gen_tcp`/`inet` manuals (packet modes, backlog, buffers), PostgreSQL's
extended-query protocol documentation, the `epgsql` 4.8 source, the `zlib` manual, and Rick
Reed's WhatsApp scaling talks. The Linux TCP limits are the ones in `box/setup-app-box.sh`;
the `tcp_rmem`/`tcp_wmem` behaviour is documented in the kernel IP sysctl manual. No response,
query-result or JWT caches are used anywhere.
