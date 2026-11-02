# css-chromium-0x95: the feed API, computed by CSS in headless Chromium

**This is a meme submission that is still correct.** It passes `test/run.sh` (42/42), and the application logic of the
feed API (routing, `:id` parsing, `Bearer` check, base64url, JWT header/payload JSON parsing, **HMAC-SHA256**, `alg` / `exp` /
`nbf` / `sub` / `username` checks, request-body JSON parsing, `TrimSpace`, UTF-16 length, escape decoding, error
precedence, status codes, error messages, query choice, `created_at` formatting, uptime) is evaluated by the **style engine of
chrome-headless-shell 155** from a generated 8 MB stylesheet. A thin Rust process does the byte I/O, and **the page itself speaks the Postgres wire protocol** (a small JS client, `web/pg.js`, through a dumb WebSocket-to-TCP relay) and returns complete raw HTTP responses. It is not fast. It is
exactly as fast as `getComputedStyle` lets it be (see "Measured costs"): about 6,000 users on a 14-core Mac under Rosetta, with
the load generator on the same machine. Nobody should build this. It was fun.

| | |
|---|---|
| Language / stack | Rust 1.94.0 (edition 2024) front, Python 3 (stdlib only, Ubuntu 24.04's 3.12) CSS generator, ~600 lines of JS (`web/glue.js`, `web/pg.js`: a Postgres v3 client) |
| Browser | Chrome for Testing **chrome-headless-shell 155.0.8059.79** (linux64, sha256-pinned in `install.sh`) |
| Rust crates (`Cargo.lock`, `--locked`) | tokio 1.54.1 only (httparse is gone: Rust no longer parses HTTP). No HTTP framework, no WebSocket crate (200 lines in `src/ws.rs`), no JSON crate, no Postgres crate |
| Database | PostgreSQL 18, exactly the queries of SPEC.md, one per request, sent by the page (prepared once per connection, binary format, pipelined). Authentication: trust, cleartext, MD5, SCRAM-SHA-256 (WebCrypto); no TLS |
| Build | `install.sh` (apt libs, rustup 1.28.2 + Rust 1.94.0 into `/opt/css-rust`, Chrome into `/opt/css-chromium`), `build.sh` (`python3 gen/build.py` writes `web/server.css`, `cargo build --release --locked`), no network at build time except the pinned crates |

## How it works

```
 client ──HTTP/1.1──▶ Rust front (tokio) ──WebSocket (batched)──▶ N × chrome-headless-shell  (one page each: glue.js + pg.js + server.css)
 (keep-alive)             │  ▲   ▲  raw HTTP response bytes ◀──────┘  insert HTML ▸ style recalc ▸ read counter-reset ▸ ...
                          │  │   └──── /pg WebSocket: opaque bytes ◀──▶ the page's own Postgres connections (Bind/Execute/Sync of the query the CSS chose)
                          ▼  ▼
                       byte relay (WS frames ◀──▶ TCP)  ◀──▶  PostgreSQL
```

A request becomes a **chain of nested elements**, each a byte window of the request carried as inline custom properties
(`<b class='A A0' style='--rid:7;--c0:101;--c1:121;…'>`):

```
q  ▸ R (route: request-target bytes, 8 per element) ▸ A (Authorization value, 32 per element)
     ▸ S (SHA-256 message blocks of the JWT signing input, 64 bytes each) ▸ O (outer HMAC block)
     ▸ B (body, 16 bytes per element) ▸ V (verdict)  ▸ [stage 3: D ▸ r × rows]
```

Everything a stage decides is a registered inherited custom property that flows down to the next descendant. The verdict element `V`
exposes the decision as numbers (`act` = which SQL to run, HTTP status, error message index, the **8 big-endian bytes of the post id and of the user id**
(the Bind parameters), the byte range of the username in the token). The page frames those bytes into a Bind/Execute/Sync, sends them over its own
Postgres connection, parses the answer into raw facts and hands them back to the CSS as a second HTML item (`D`: SQLSTATE as 5 character codes, the
CommandComplete tag as character codes; `r` per row: the id, the `timestamptz` and the `like_count` as raw bytes). The CSS **interprets** them: success or
error, the affected-row count (it parses the digits of the tag), foreign-key violation 23503, the final kind/status/message, the id and like count
as integers, the date/time of `created_at` from the 8 timestamp bytes. The page fills the JSON and HTTP-head templates that live on the stylesheet and
sends the finished raw HTTP response to the front, which just writes it. A request takes two round trips through the page (stage 1, stage 3), or
one when CSS answers by itself (404, 400, 401: no database involved).

### What is in CSS, what is in Rust, what is in JS

| Where | What |
|---|---|
| **CSS** (`gen/*.py` → `web/server.css`) | **All decisions, HTTP framing included** (the *frame pass*, `gen/http.py`: request line, header grammar and header-name matching of httparse, `Content-Length`, `Connection`, `Expect`, `Transfer-Encoding`, first `Authorization`, head end, 64-header limit; fast path = attribute selectors over the head, ~70 µs per light GET; slow path = a per-byte automaton, ~11 ms for the 300 B head of a like). Route table and `:id` (`ParseInt` > 0) automaton; `Bearer ` prefix; base64url decode; JSON state machine over the JWT header and payload (full RFC 8259 grammar, escapes, duplicates, nesting ≤ 18); `alg == HS256`, `exp`, `nbf`, `sub` (`TrimSpace` + `ParseInt`), `username`; **HMAC-SHA256** (the key lives on `:root`, ipad/opad midstates are computed once) and the constant-length signature compare; request-body JSON parser, unescape (surrogate pairs, U+FFFD), `TrimSpace`, UTF-16 length ≤ 500; error precedence (404 → 401 variants → 400 variants); status codes; message indices; which SQL; interpretation of the DB outcome (health ok/degraded, 404 for 0 rows, 23503 → 404 on a like, 200 vs 201 for an existing like); date arithmetic for `created_at`; uptime |
| **Rust front** (`src/`) | **Byte I/O, no decisions, no SQL, no JSON, no HTTP parsing** (honest list): accepts on `PORT`/`EXTRA_PORT`; reads bytes into a bounded per-connection buffer (head window <= 2 KB, body <= 48 KB); sends the first bytes of the buffer to a light page as a **frame pass** and **slices by the numbers the CSS returns** (head length, Content-Length, close, method type, request-target range, Authorization range, `Expect` -> writes `100 Continue`; incomplete -> read more and ask the CSS again only once a new LF byte has arrived, at most 66 times per head, so a byte-at-a-time sender can't burn the light page (completeness is still the CSS's call); bad -> close; head + body capped at 2 KB + 64 KB whatever the CSS says); for that it emits, besides the bytes as decimal custom properties, three strings that are *stateless per-byte table lookups* (the byte as a character, its character class `t`/`u`/`:`/`s`/`r`/`n`/`!`, and the byte with digits written as `9`) for the CSS attribute selectors; keeps leftover bytes for the next frame pass (pipelining); assigns request ids; picks the pool by *whether the CSS-given Authorization range is non-empty*; queues, batches, rejects with 503 when a queue is full (that 503, and the 500 for a dead page, are the only responses it writes itself, besides `100 Continue`); WebSocket server for the pages; supervises and respawns Chromium; **the Postgres relay** (`/pg` WebSocket: opens a TCP connection to the host:port of `DATABASE_URL` and copies bytes both ways without looking at them; hands user/password/database to the page); supplies `--now`, `--start` (the clock is an input); writes the response bytes the page produced. The WebSocket upgrade request of its own pages is read with a 15-line header scan (`src/ws.rs`). |
| **JS** (`web/pg.js`, `web/glue.js`) | Moves and serialises data, decides nothing about the application. `pg.js`: Postgres v3 framing and parsing (startup, trust / cleartext / MD5 / SCRAM-SHA-256 with WebCrypto, Parse of the 5 statements once per connection, Bind/Execute/Sync in binary format, many requests per write, `ReadyForQuery` delimits an answer, ErrorResponse → SQLSTATE), reconnect with backoff, 10 s query timeout, which connection gets a request (least in flight). `glue.js`: inserts the HTML, reads `getComputedStyle(...).counterReset` (exact int32 outputs), strips sentinel digits, concatenates the decoded username/body code points the CSS emitted per byte position, UTF-8 encodes the body parameter and decodes DB strings, writes the raw result bytes back as integer properties (zero bytes omitted), JSON-escapes DB strings (Go `encoding/json` parity), fills the `%x` markers of the CSS-owned templates, counts the body bytes for `Content-Length`, packs the binary messages to the front (frame results are 13 numbers read off the `Z` element), removes finished chains, calls `gc()`. Choices that are JS's: which statement columns are id / timestamp / like count (fixed per statement), which Postgres connection to use, the text of connection errors (`error` of the degraded health body), "database down" = every connection is backing off |

### CSS techniques

* **Registered `@property`** for ~26,000 custom properties (typed `<integer>`, initial values, `inherits` chosen per property: only the module
  boundary props inherit, the intermediate ones are non-inherited so each element computes them once).
* **Parity chaining over nested elements.** A stage that needs "what the previous element computed" cannot read its own earlier value
  (cycle), so chunk elements alternate classes (`A0`/`A1`, `S0`/`S1`, `B0`/`B1`, `R0`/`R1`): the `A1` rule reads the `--*-a` state of its parent
  and writes `--*-b`, the `A0` rule the reverse. A SHA-256 compression is one such element; a JWT is a chain of 3 inner + 1 outer blocks.
* **Container style-query gating.** Heavy rules sit in `@container style(--need-auth: 1)` / `style(--need-body: 1) and style(--au: 3)`.
  A `GET /feed` that happens to carry a token, or a bogus body on a route that does not read one, costs ~0.03 ms instead of ~200 ms.
* **16-bit-half SHA-256.** A registered value that crosses a `var()` hop is exact only to **6 significant digits** in Chrome 155, so a 32-bit word is
  two 16-bit halves (`--x<k>l`, `--x<k>h`), bits are extracted once into their own props (`mod(round(down, x / 2^k), 2)`), XOR3 is `mod(a+b+c, 2)`,
  Ch is `g + e*(f-g)`, Maj is `round(down, (a+b+c)/2)`, additions are weighted sums re-split with `mod` / `round`. Exact against `hashlib`, `node:crypto`
  and the 60 seed tokens' own signatures.
* **The 6-significant-digit limit** shapes everything: ids and user ids are base-1e6 limbs; inline `<integer>` *literals* from Rust are the one
  exception (exact to 2^31-1) and carry `--now`, `--ts-hi`; timestamps are two limbs and recombined only inside a single `calc()`.
* **Outputs via `counter-reset`.** Custom properties lose digits on `getPropertyValue`, but `counter-reset: name <integer> …` serialises exact int32. Each
  output element lists its `name value` pairs in one declaration; the glue parses `getComputedStyle(el).counterReset`.
* **Defeating Blink's matched-properties cache with `--rid`.** Two elements with byte-identical inline style and identical parent state share one
  computation (which would make a second verification of the same token free; this is also the "no remembered JWT verifications" rule). Every
  element of every chain carries its own unique `--rid`, and `:root` is never mutated after init (that would recompute every live chain).
* **`display:none` container.** Same style cost as a visible one, zero layout and paint; computed style is lazy per element chain that is read.
* **Cost-aware rules.** A declaration costs ~7 ns per byte of its text plus ~0.6 µs per `var()`; registered-property count does not matter. Hot rules are small,
  the route automaton runs on 8-byte chunks and only for `/posts/…`, body and signature work is gated.

## Pools, scheduling, memory (`src/hub.rs`)

Authenticated requests are ~1000× more expensive than anything else: one HMAC chain is ~4 SHA-256 compressions of ~30 ms each, plus ~85 ms
for parsing the token, all on the page's single thread. So:

* Workers are split into a **heavy pool** and a **light pool**. The *only* scheduling input is structural: a request with a non-empty
  `Authorization` header is heavy (its chain contains the SHA blocks), everything else is light. Rust never looks at what CSS decided.
* Each class has **one shared FIFO queue** (a request is not bound to a worker until one is idle: no per-worker backlog to get stuck behind).
  One WS message in flight per worker (until the page's ack, which comes as soon as stage 1 is computed). A heavy worker takes **one** heavy request per message (batching only delays the first answer: cost is linear in
  the batch); a light worker takes everything queued (≤ 256).
* **The database round trip happens inside the page** (no front message for it). A request the CSS sent to Postgres is acked as "pending" and finishes with its
  own message; a heavy worker with pending requests is held back for up to `CSS_HOLD_MS` (8 ms) so that a heavy request's second half (the answer arrives as an event of the page,
  which is busy while it computes) never waits for the next ~250 ms heavy request.
* An idle heavy worker **steals light requests** (batches of 8) when the light worker is busy. Light workers never take heavy work.
* **Overload:** a full queue (`2.5 × heavy workers` heavy requests ≈ 0.8 s of waiting; 20,000 light) answers `503 {"error":"server busy"}` at once; requests whose client has
  gone are dropped from the queue unprocessed. A request that waits for a dead page is failed with 500 (written by the front). A page's Postgres connections die with it (the relay closes the TCP side) and come back with the respawned page. Heavy pages run at `nice 10` so the front and the light page keep their CPU.
* **Why latency looks like this:** at ~8 % authenticated requests the 95th percentile of *all* requests is the ~37th percentile of the heavy ones,
  so p95 ≈ the heavy service time (~250–320 ms under load) as long as queues stay short, and p99 is the 87th percentile of the heavy ones.
* **Memory:** one SHA-256 block's computed style is ~5.5 MB of custom properties that Blink frees only when its heap is collected. Without a GC an
  authenticated request leaves ~22 MB behind (60 requests: 400 MB → 1.7 GB in one page; a 3-minute run with 4,000 users: 7.7 GB total). Chrome is started with
  `--js-flags=--expose-gc` and the glue calls `gc({type: 'major', execution: 'async'})` after every heavy request has finished (for one that went to Postgres: when its answer is built and its chain removed) (a synchronous `gc()` costs ~40 ms of the
  page's only thread per request; the async one returns in ~1 ms and marks/sweeps on helper threads). Total PSS of front + all Chromium stays flat
  (see results). Pages that only see light requests collect every 20,000.
* **Chunk sizes** (`gen/constants.json`): the cost of a chunk element is proportional to its position count, not to the bytes used. `body_chunk` 64 → 16 takes an 85-char
  body from 30 to 21 ms; `auth_chunk` 64 → 32 saves ~8 % per authenticated request.

## Configuration (all optional; the contract env vars are DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT)

| Env | Default | |
|---|---|---|
| `CSS_HEAVY` | `max(1, nproc - 2)` | heavy pages (authenticated requests). 14 on the 16-core box, 6 on 8 cores, 2 on 4 cores |
| `CSS_LIGHT` | 1 (2 when nproc ≥ 12) | light pages |
| `CSS_WORKERS` | | total pages; if set without `CSS_HEAVY`, heavy = `CSS_WORKERS - CSS_LIGHT` |
| `CSS_HEAVY_BATCH` / `CSS_LIGHT_BATCH` / `CSS_STEAL_BATCH` | 1 / 256 / 8 | requests per WS message |
| `CSS_HOLD_MS` | 8 | how long a heavy page waits for the database round trip of its pending request before taking the next heavy one |
| `CSS_QUEUE_HEAVY` / `CSS_QUEUE_LIGHT` | `2.5 × heavy` / 20000 | queue limits, beyond which 503 |
| `CSS_HEAVY_NICE` | 10 | nice value of heavy Chromium processes |
| `CSS_GC_EVERY` / `CSS_GC_LIGHT_EVERY` | 1 / 20000 | `gc()` after this many heavy / light requests (0 = never) |
| `CSS_STATS=1` | off | scheduler counters on stderr every 10 s |
| `PG_CONNS` | 32 | Postgres connections in total (cap 512, rule 5). The pages open them: every heavy page 1, the light pages share the rest (22 with 10 heavy + 1 light). The relay refuses a 513th. `DATABASE_URL` is parsed for host, port, user, password (percent-decoded), database; no TLS, no unix sockets |
| `CSS_WS_PORT`, `CHROME_BIN`, `FRONT_THREADS`, `CSS_CHROME_LOG` | 9900, `/opt/css-chromium/chrome-headless-shell`, 2, off | |

Startup: `/health` answers ~2 s after `start.sh` (the first page is ready; the others keep loading). Idle keep-alive connections are never timed out by the
front (tested: a connection idle for 100 s still works); thousands of connections are cheap (an idle one owns no buffer: 8,000 connections = 23 MB of front RSS).
If a Chromium dies or its page disconnects, its in-flight requests get 500 and it is respawned within ~1 s (tested by killing Chromium processes under load).

## Deviations from the Go reference (`examples/go/handlers.go`)

Everything else is meant to be identical (local test suites, kept out of this PR, compared the CSS with a Go reference on 444 token cases, the route automaton on 2140 paths, the body parser on 688 bodies, the date formatter on 4029 timestamps).

* **Authorization / JWT** (`gen/auth.py`):
  1. leading whitespace after `Bearer ` makes the token invalid (the SHA blocks start at byte offset 7);
  2. only ASCII whitespace is trimmed from the token; in `sub` strings Unicode white space written as `\uXXXX` is trimmed like Go, raw UTF-8 Unicode spaces (U+00A0 as `C2 A0`) are not;
  3. JSON nesting deeper than 18 is rejected (Go: 10000);
  4. `sub` as a JSON number must be plain digits (`7.0`, `1e3` are rejected; Go accepts them); `sub` values with more than 17 significant digits are rejected; user ids ≥ 10^12 are clamped to 999999999999 (no such user exists);
  5. `exp` / `nbf` are compared on their first 10 significant digits (assumes 1e9 ≤ now < 1e10); the float64-overflow window 1.797e308 … 1e309 is accepted, and integer-overflow handling starts at 1e19 (Go: 9.22e18);
  6. an embedded CR/LF in the token (impossible over HTTP) is invalid here, ignored by Go's base64 decoder.
* **Request body** (`gen/body.py`): raw NUL bytes are treated as padding; invalid UTF-8 may yield a different number of U+FFFD than Go in rare cases; numbers beyond float64 range are accepted (Go: error); JSON nesting ≤ 18.
* **Routing** (`gen/route.py`): post ids ≥ 2^53 are approximate (the 2^63-1 boundary is still rejected correctly).
* **Dates** (`gen/stage3.py`): the 8 timestamp bytes become days / second of day / millisecond inside single `calc()`s, so `created_at` is exact up to 2^53 µs (year 2285; the old 2068 limit is gone); milliseconds are truncated like Go's `.000` layout; timestamps before 2000 are clamped to the epoch.
* **Ids** (Part P): the row `id` and `like_count` leave the CSS as an exact int32 (`counter-reset`): post ids ≥ 2^31 are clamped (the seed has 500,000 posts); post ids ≥ 10^12 given in a URL are approximate in the Bind parameter (the route stage keeps 6 significant digits of the high limb; no such post can exist).
* **Postgres** (Part P): no TLS (`sslmode` is ignored), SCRAM without SASLprep (ASCII passwords are unchanged), MD5 implemented in JS, a username with invalid UTF-8 in the token payload is re-encoded with U+FFFD (it was copied verbatim), the `error` text of a degraded `/health` is the connection error (`postgres unavailable: connect pg:5432: Connection refused ...`).
* **Transport / limits** (Rust): requests larger than 48 KB and `Transfer-Encoding: chunked` request bodies close the connection; no gzip (optional in SPEC); `JWT_SECRET` must be ≤ 64 bytes (the HMAC key is one block; the server refuses to start otherwise); a full queue answers 503 `{"error":"server busy"}`; a 40 KB body costs a page ~2 s before it is rejected (bounded, not quadratic).

## Measured costs (Chrome 155, linux/amd64 under Rosetta on Apple silicon; native x86 should be faster)

| Request | In-page CSS cost |
|---|---|
| 404, 400 `:id`, missing `Authorization` header (stage 1 only) | 10–40 µs |
| `GET /feed` (20 rows) incl. stage 3 (id, created_at and like count come from raw bytes now) | ~1.6 ms (V 0.1 + stage 3 1.5; was ~0.7) |
| `GET /posts/:id` | ~0.4 ms |
| request with a token that fails before the HMAC (expired, malformed payload): the token parse still runs | ~85 ms |
| authenticated like (A stage ~85 ms + HMAC ~110–130 ms, 3 inner + 1 outer compression) | 200–230 ms (≈ 4.5/s per page) |
| `POST /posts` adds body parsing | 3 ms (tiny) – 21 ms (mload's ~85 chars) – 125 ms (500 chars) – 265 ms (500 two-/four-byte characters) |
| one SHA-256 compression | ~30 ms |

## Local load test results

`mload` (`-gzip -think-scale 2.5`, the scoring mix) against `start.sh`, **on a Mac with 14 cores shared with the load generator, the Postgres container
and a Rosetta-emulated amd64 box** (OrbStack, 12 GB VM). Absolute numbers are *not* the benchmark box's; native x86 with a separate load generator should hold clearly more.
Config: `CSS_HEAVY=10 CSS_LIGHT=1` (the defaults would be 12 + 2 here, but 4 cores are busy with the load generator and Postgres), everything else default (batch 1, hold 8 ms, nice 10, queue 25, async gc).
"CPU" = busy share of all 14 cores, "PSS" = peak summed proportional set size of the front and all Chromium processes (idle: ~3.0 GB for 11 pages).

| Users | Ramp / hold | req/s | p50 | p95 | p99 | Errors | CPU | peak PSS | |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---|
| 3,000 | 30 s / 2 m | 125 | | 280 ms | 319 ms | 0 | 22 % | 3.4 GB | pass |
| 4,000 | 30 s / 2 m | 167 | | 294 ms | 385 ms | 0 | 32 % | 3.6 GB | pass |
| 5,000 | 30 s / 2 m | 209 | | 294 ms | 504 ms | 0 | 39 % | 3.6 GB | pass |
| 5,000 | 60 s / 2 m | 190 | | 294 ms | 342 ms | 0 | 38 % | 3.6 GB | pass |
| 5,500 | 60 s / 2 m | 210 | 2.9 ms | 300–312 ms | 363–504 ms | 0 | 43 % | 3.6 GB | pass (two runs) |
| 6,000 | 60 s / 2 m | 229 | | 306 ms | 484 ms | 0 | 50 % | 3.7 GB | pass |
| 7,000 | 60 s / 2 m | 268 | | 338 ms | 1,073 ms | 0.12 % (503) | 60 % | 3.8 GB | fail (p99) |
| 8,000 | 60 s / 2 m | 304 | | 366 ms | 1,162 ms | 0.27 % (503) | 65 % | 3.7 GB | fail (p99) |
| **5,500** | **60 s / 5 m** | 221 | | 306 ms | 442 ms | 0 | 54 % | 3.7 GB | **pass (5-minute hold)** |
| 6,000 | 60 s / 5 m (earlier build) | 241 | | 356 ms | 1,052 ms | 0.06 % (503) | 74 % | 3.7 GB | fail (p99, marginal) |
| | | | | | | | | | **with Part P (the page speaks Postgres; same box, same settings)** |
| 5,000 | 60 s / 2 m | 192 | | 264 ms | 366 ms | 0 | | | pass |
| 5,500 | 60 s / 2 m | 210 | | 248 ms | 297 ms | 0 | | | pass |
| 6,000 | 60 s / 2 m | 229 | | 256 ms | 322 ms | 0 | | | pass |
| **5,500** | **60 s / 5 m** | 221 | | 253 ms | 303 ms | 0 | ~50 % | 3.9 GB | **pass (5-minute hold)** |
| **6,000** | **60 s / 5 m** | 242 | | 259 ms | 316 ms | 0 | ~54 % | 3.9 GB | **pass (5-minute hold)** |
| | | | | | | | | | **with Part H (Rust no longer parses HTTP: every request first goes through a frame pass on the light page)** |
| 5,500 | 60 s / 2 m | 210 | | 280 ms | 352 ms | 0 | ~50 % | 3.8 GB | pass |
| 6,000 | 60 s / 2 m | 228 | | 274 ms | 374 ms | 0 | ~55 % | 3.7 GB | pass |
| **6,000** | **60 s / 5 m** | 241 | | 283 ms | 363 ms | 0 | ~58 % | 4.1 GB peak, 3.9 GB steady | **pass (5-minute hold)** |

* **Best passing: 6,000 users for a 5-minute hold with Part P** (before it: 5,500 for 5 minutes, 6,000 for 2 minutes; a 5-minute run at 6,000 missed p99 by 5 %). p99 varies by ±30 % from run to run, so read the Part P rows as "not worse, probably a little better" (p95 down from 300–306 to 248–264 ms, p99 at 5,500/5 m from 442 to 303 ms): the front no longer does a database round trip, a JSON render and two channel hops per request, and the page's Postgres answers are handled as soon as they arrive. 7,000 was not re-measured. The limit is the heavy pool: ~8 % of requests are authenticated
  (likes and posts, ~320 ms of one page each *under load*, ~220 ms idle), 10 pages ≈ 30 authenticated requests/s, and mload's phases are synchronised after the ramp, so the
  likes arrive in waves. Past the limit the queue cap turns the overflow into fast 503s instead of 5-second waits (before the cap: 8,000 users gave p99 = 6 s).
* **p95 ≈ the heavy service time** (see above), p50 is 3 ms (feed/post requests never wait for a heavy one: they have their own page and queue).
* **Memory** (Part P: fresh server, 5,500 users, 5 m): PSS goes 2.85 GB (idle) → 3.45 GB after one minute → 3.9 GB at the end of the hold (before Part P: 3.5–3.65 GB peak). It is a warm-up of lazily built style data, not a leak: after the run the server idles at 3.15 GB, after a second 3-minute run 3.44 GB, after a third 3.53 GB (+300 MB, then +90 MB). Before Part P: 2.9 GB (idle) → 3.5–3.65 GB and stays there (front: 16 MB at 5,500 connections, 23 MB at 8,000). Without the GC calls the same load grew to
  7.7 GB in 3 minutes; the synchronous `gc()` alternative is 15 % slower.
* The front uses ~5 % of one core at 5,500 users (20 s of CPU in 6 minutes), Postgres is not a factor. The 14-core VM was 54 % busy, i.e. the Rosetta-emulated pages and `mload` are the cost.
* Heavy pool sizing (30 s ramp, 6,000 users): 10 heavy pages beat 13 on this box because the extra pages only compete with the load generator; on the real 16-core box the default
  `nproc - 2` heavy pages leave 2 cores for the front, Postgres client work, kernel networking and the light pages. The ratio that maximises users is "as many heavy pages as spare cores": the light pool
  needs one page (~1,400 req/s capacity) at any size this server can hold.
* Other tried levers: `CSS_HEAVY_BATCH=2` (no difference: batches rarely coincide), nice 10 on heavy pages (p95 at 6,000 users from 862 to 345 ms in an overloaded run), queue cap 16 → 2.5 per heavy page
  (p99 1.7 s → 0.86 s at 6,000), async vs. synchronous vs. no GC (above).

* **Part H cost** (frame pass; in-page, Rosetta): a light GET (`/feed`, `/posts/N`, `/health`) is framed by attribute selectors in **~70 µs** (phase 1: one `Hs` + `Z`), a like or `POST /posts` head (~300-400 bytes with the JWT) needs the per-byte automaton, **~11-13 ms** on the light page
  (phase 2; about 18 per second at 5,500 users = ~20 % of the light page, the heavy pages are not charged unless they steal it). Before the selector tier and the cascade fix a light GET cost ~2.7 ms and a like ~22 ms; the lessons
  are in `gen/http.py`: declaration *text* costs ~8-28 ns per byte whether or not a branch is taken, an `@container` rule that loses still costs its text, and `style(--x: N)` inside `@container` does not match a calc()-produced value (range queries do).
  Measured throughput/latency is within the run-to-run noise of Part P (p95 +10-30 ms); the light GC weight of a phase-2 frame is 200 requests, memory is unchanged (3.9 GB steady, 4.1 GB peak).
  Correctness: 2,159 head windows (mload's and test.sh's exact bytes, every prefix of several requests, header-name case variants, duplicates, folding, bare LF, NUL, 64/65/100 headers, long heads, CL values, pipelining, crafted colliding names, malformed request lines with a version-like header value) give the same
  `ok bad hl used close cont mt ps pe as ae` as httparse 1.10.1 + the old `parse()`, except for the 16 documented deviations (`gen/http.py`).

The containerised dev environment (Postgres + an amd64 Ubuntu box, unit suites, load-test scripts) was used for all of the above and is kept out of this PR.

The CI run (fresh `ubuntu:24.04` linux/amd64 container pinned to 4 CPUs, `install.sh` as root ~50 s, `build.sh` as a normal user ~45 s with only the pinned crates downloaded, `test/run.sh` against Postgres 18) passes **42/42**.

## Layout

```
install.sh build.sh start.sh   toolchain (root) / build (user) / run
src/                           Rust front: http.rs (framing), fragment.rs (request → HTML), hub.rs (pools, queues, protocol), wsrv.rs + ws.rs (pages),
                               relay.rs (WS ↔ TCP byte relay to Postgres), chrome.rs (supervisor), config.rs (env, DATABASE_URL)
web/                           index.html + glue.js (data mover, response assembly) + pg.js (Postgres client); server.css is generated by build.sh
gen/                           CSS generator, Python stdlib: route, auth (+ sha256, hmac_sha256), body, verdict, stage3, templates (JSON/HTTP templates as strings), common, build; constants.json
```
