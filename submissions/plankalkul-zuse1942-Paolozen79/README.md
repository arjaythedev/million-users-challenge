# Plankalkül (Konrad Zuse, 1942–1945) — `plankalkul-zuse1942-Paolozen79`

An implementation of the Million Users Challenge Feed API in **Der Plankalkül** (*"The Calculus of Plans"*), designed by **Konrad Zuse** between **1942 and 1945** — the world's first high-level programming language, predating Fortran by over a decade.

Both of Zuse's notations are compiled from source during `build.sh` using [`Zaneham/plankalkul-compiler`](https://github.com/Zaneham/plankalkul-compiler) (`plankalkul --emit-c`) and linked with a share-nothing `epoll` + pipelined `libpq` + Intel `ISA-L` `igzip` runtime bridge (`src/runtime.c`):

1. **Original 1942 2D 4-Row Grid Notation ([`src/zuse_1942.pk2d`](src/zuse_1942.pk2d))** — Zuse's vertical 4-line blocks (` |` expression row, `V|` variable index row, `K|` component row, `S|` structure/type row) and `Randauszug` headers (`( V ) => ( R )`).
2. **Linearised Plankalkül + 1941 *Wahrheitstafeln* ([`src/feed_plans.pk`](src/feed_plans.pk))** — a Zuse decision table (`TABLE route_policy`), `P Delta 1` .. `P Delta 7` plans, `W` / `W1` loops, `->` conditionals and `=>` assignments.

---

## Stack & Pinned Versions

| Component | Version / Source | Role |
|---|---|---|
| **Language** | **Plankalkül** (Konrad Zuse, 1942–1945) | Routing decisions, `:id` parsing, JS whitespace trimming & codepoint counting, JWT claim decisions, timestamp calendar arithmetic, pool sizing, batch pacing |
| **Plankalkül Compiler** | [`Zaneham/plankalkul-compiler`](https://github.com/Zaneham/plankalkul-compiler) @ `a3bc749a` (OCaml + Dune from Ubuntu packages, SHA-256 pinned) | Compiles `.pk2d` and `.pk` to C (`--emit-c`) |
| **C Compiler** | `gcc` (Ubuntu 24.04, `-O3 -march=native -flto=auto -fno-plt -Wl,--gc-sections`) | Compiles the generated C plans + `src/runtime.c` with LTO and dead-section stripping |
| **Postgres Client** | static `libpq` from **PostgreSQL 18.6** source (SHA-256 pinned, built without OpenSSL) | Binary-format prepared statements in **pipeline mode** |
| **Gzip** | **Intel ISA-L 2.31.1** `igzip` (SHA-256 pinned, static) | Stateless level-0 deflate for responses ≥ 1024 B when the client accepts gzip |
| **Event Loop / HTTP / JWT** | `src/runtime.c` | Thread-per-core `epoll`, RFC 8259 JSON, HS256 (scalar SHA-256, constant-time compare) |

---

## What Runs in Plankalkül vs. the C Runtime

### 1. 1942 2D Grid Notation Plans (`src/zuse_1942.pk2d`)

```text
 | zuse_pool_split ( V , V ) => ( R , R )
V|                   0   1        0   1
K|
S|                   i   i        i   i
```

- **`zuse_pg_us_split(V0) => (R0, R1, R2)`**: Splits a binary `TIMESTAMPTZ` (µs since 2000-01-01) into Unix days, second-of-day and microseconds.
- **`zuse_civil_date(V0) => (R0, R1, R2)`**: Days since 1970-01-01 → proleptic Gregorian `(year, month, day)` (400-year cycle).
- **`zuse_clock_hms(V0, V1) => (R0, R1, R2, R3)`**: Second-of-day + µs → `(hour, minute, second, millisecond)`.
- **`zuse_like_status(V0) => R0`**: Maps the like insert's affected-row count to the HTTP status.
- **`zuse_uptime(V0, V1) => R0`**: Server uptime in seconds for `GET /health`.
- **`zuse_pool_split(V0, V1) => (R0, R1)`**: Per-thread pool size `R0 = total / threads` and write connections `R1 = R0 / 4`. The runtime passes `total = min(16 × threads, 128)`, so e.g. 16 threads → 8 connections each (6 read + 2 write), 128 total.

### 2. Linearised Plans & *Wahrheitstafel* (`src/feed_plans.pk`)

- **`TABLE route_policy`**: decision table `(is_get, is_post, is_like_path) → (valid_subpath, requires_jwt)` for `/posts/:id` and `/posts/:id/like`.
- **`P Delta 1 parse_post_id`**: validates a positive decimal integer of at most 18 digits (no overflow).
- **`P Delta 2 js_ws_len_at` / `P Delta 3 js_ws_len_before`**: every UTF-8 whitespace sequence recognised by JavaScript `String.prototype.trim()`.
- **`P Delta 4 trim_and_count_body`**: trims the post body and counts code points → ok / empty / > 500.
- **`P Delta 5 verify_jwt_claims`**: given the decoded claims (signature, `alg`, `exp`/`nbf` are checked in C), decides *invalid or expired* vs *invalid payload* and parses `sub`.
- **`P Delta 6 route_http_request`**: method + path → action (dispatch / 400 / 404 / auth-first) and query kind; enforces *auth before id* (`SPEC.md`).
- **`P Delta 7 batch_pause_ns`**: evaluates batch pacing threshold (`BATCH_NS = 0` so worker threads never call `nanosleep()`).

### 3. Runtime Bridge (`src/runtime.c`)

- **Thread-per-core `epoll`**: one worker thread per CPU in the affinity mask, each with its own `epoll` (`evs[2048]`), its own `SO_REUSEPORT` + `TCP_DEFER_ACCEPT` listeners on `PORT` and `EXTRA_PORT`, and its own Postgres connections. No locks in the request path.
- **Transparent Huge Pages (`MADV_HUGEPAGE`) & 40-byte connection slots**: `conns`, `arena`, and `zbuf` are 2 MB aligned and backed with `MADV_HUGEPAGE` to minimize `dTLB` misses and 4K page walks; idle keep-alive connections hold no heap buffers.
- **SSE2 SIMD JSON Escaping & 2-Digit Lookup Table (`DEC2`)**: `putjs()` scans 16 bytes at a time via SSE2 (`_mm_max_epu8` + `_mm_cmpeq_epi8`) for characters requiring JSON escaping, and `putu()` / `put_ts()` format integer IDs and Plankalkül calendar timestamps using 2-digit table lookups.
- **Fair Least-Loaded Pipelined `libpq`**: read queries are packed up to `PACK = 16` per `PQsendPipelineSync()`, immediately flushed mid-turn once a batch reaches 16, and balanced so no connection exceeds the least-loaded connection by more than `PACK`. `pg_read()` drains `PQconsumeInput()` / `PQgetResult()` until no complete results remain. Writes (`POST /posts`, likes) each get their own sync and are answered only after `PGRES_PIPELINE_SYNC`.
- **Fault isolation**: if one read query in a batch fails, the others (reported as `PGRES_PIPELINE_ABORTED`) are re-sent once instead of failing. Lost Postgres connections are re-established by a helper thread, so the event loop never blocks on `connect()`.
- **HTTP hardening**: malformed requests (bad request line, `Transfer-Encoding`, oversized headers) get `400` and the connection is closed; the response buffer is bounds-checked (overflow → `500`).
- **Gzip**: Huffman tables are built **once at startup from a fixed, hard-coded sample** of feed-shaped JSON — nothing derived from database data or served responses is kept (Rule 7).
- **No caching across requests**: every request runs its query against Postgres.
