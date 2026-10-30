# Plankalkül (Konrad Zuse, 1942–1945) — `plankalkul-zuse1942-Paolozen79`

An implementation of the Million Users Challenge Feed API in **Der Plankalkül** (*"The Calculus of Plans"*), designed by **Konrad Zuse** between **1942 and 1945** in Berlin and Hinterstein (Allgäu) — the world's first high-level programming language, predating Fortran by over a decade.

Both of Zuse's notations are compiled from source during `build.sh` using [`Zaneham/plankalkul-compiler`](https://github.com/Zaneham/plankalkul-compiler) (`plankalkul --emit-c`) and linked with a share-nothing `epoll` + pipelined `libpq` + Intel `ISA-L` `igzip` runtime bridge (`src/runtime.c`):

1. **Original 1942 2D 4-Row Grid Notation ([`src/zuse_1942.pk2d`](src/zuse_1942.pk2d))** — Uses Zuse's vertical 4-line blocks (` |` expression row, `V|` variable index row, `K|` component row, `S|` structure/type row with `A10` signed integers) and `Randauszug` headers (`( V ) => ( R )`).
2. **Linearised Plankalkül + 1941 *Wahrheitstafeln* ([`src/feed_plans.pk`](src/feed_plans.pk))** — Uses Zuse's 1941 boolean decision tables (`TABLE route_policy`) and hierarchical `P Delta` plan groups (`P Delta 1` .. `P Delta 7`), `W` / `W1` iteration loops, `->` conditionals (`Bedingte Pläne`), and `=>` *Ergibt-Zeichen* assignments.

---

## Stack & Pinned Versions

| Component | Version / Source | Role |
|---|---|---|
| **Language** | **Plankalkül** (Konrad Zuse, 1942–1945; *Berichte der GMD Nr. 63*, 1972) | Business logic, HTTP routing table, `:id` parsing, JS UTF-8 whitespace trimming & codepoint counting, JWT claim validation, UTC timestamp calendar arithmetic, and adaptive write-batching logic |
| **Plankalkül Compiler** | [`Zaneham/plankalkul-compiler`](https://github.com/Zaneham/plankalkul-compiler) @ `a3bc749a948d99c544b7e2fdcf5137e96465cc6e` (OCaml 4.14 / Dune 3.14, SHA-256 pinned) | Compiles `.pk2d` (2D grid notation) and `.pk` (linear notation + Truth Tables) to C (`--emit-c`) |
| **C Compiler** | `gcc` (Ubuntu 24.04 `build-essential`, `-O2 -march=x86-64-v2 -flto=auto -fno-plt`) | Compiles generated Plankalkül C plans + `src/runtime.c` with link-time optimization across translation units |
| **Postgres Client** | `libpq` from **PostgreSQL 18.6** source tarball (SHA-256 pinned, static `libpq.a`) | Non-blocking binary protocol (`PQsendQueryPrepared`) in **pipeline mode** (`PQenterPipelineMode`) |
| **Gzip** | **Intel ISA-L 2.31.1** (`igzip`, SHA-256 pinned, static `isa-l.a`) | Single-call stateless `isal_deflate_stateless` level 0 for `GET /feed` when `Accept-Encoding: gzip` is present and payload ≥ 1024 B |
| **Event Loop / HTTP / JWT** | `src/runtime.c` (`epoll`, `SO_REUSEPORT`, RFC 8259 JSON, HS256) | Share-nothing thread-per-core `epoll` loops, 40-byte connection slots, constant-time HMAC-SHA256 |

---

## Architecture: What Runs in Plankalkül vs. C Runtime Bridge

### 1. 1942 2D Grid Notation Plans (`src/zuse_1942.pk2d`)

Each block in `src/zuse_1942.pk2d` is written in Zuse's four-row vertical representation, where every variable `V` (input), `Z` (intermediate *Zwischenwert*), and `R` (result *Resultatwert*) has its subscript and type aligned directly in the columns below it:

```text
 | zuse_pg_us_split ( V ) => ( R , R , R )
V|                    0        0   1   2
K|
S|                    A10      A10 A10 A10
```

- **`zuse_pg_us_split(V0) => (R0, R1, R2)`**: Splits a PostgreSQL binary `TIMESTAMPTZ` (microseconds since `2000-01-01T00:00:00Z`) into Unix days (`R0`), second-of-day `0..86399` (`R1`), and sub-second microseconds `0..999999` (`R2`).
- **`zuse_civil_date(V0) => (R0, R1, R2)`**: Converts days since `1970-01-01` into proleptic Gregorian `(year, month, day)` using the 400-year civil calendar cycle (`146097` days per era).
- **`zuse_clock_hms(V0, V1) => (R0, R1, R2, R3)`**: Decomposes second-of-day and remaining microseconds into `(hour, minute, second, millisecond)` with exact truncation to milliseconds (`SPEC.md`).
- **`zuse_like_status(V0) => R0`**: Maps PostgreSQL `INSERT ... ON CONFLICT DO NOTHING` affected-row count to `1` (`"liked"`) or `0` (`"already_liked"`).
- **`zuse_uptime(V0, V1) => R0`**: Computes non-negative server uptime in seconds for `GET /health`.
- **`zuse_pool_split(V0, V1) => (R0, R1)`**: Computes per-worker database pool sizes (`64 / nworkers`), splitting connections between pipelined read connections and dedicated write connections (`1` write connection when per-worker pool `< 4`, `2` when `>= 4`).

### 2. Linearised Plankalkül Plans & 1941 *Wahrheitstafeln* (`src/feed_plans.pk`)

- **`TABLE route_policy`**: A 1941 Zuse *Wahrheitstafel* (Truth Table) mapping `(is_get, is_post, seg0_code, seg1_code)` to `(route_id, needs_auth, needs_id)`:
  - Enforces the `SPEC.md` rule that authenticated endpoints (`POST /posts`, `POST /posts/:id/like`) check `Authorization: Bearer` (`401`) **before** validating `:id` (`400`).
- **`P Delta 1 parse_post_id(V0, V1) => R0`**: Iterates over the URL segment bytes via `W1(V1) => Z0` and `c_peek_u8`, validating that `:id` is a positive integer `>= 1` with no non-digits and clamping on 64-bit overflow.
- **`P Delta 2 js_ws_len_at(V0, V1, V2) => R0`** & **`P Delta 3 js_ws_len_before(V0, V1, V2) => R0`**: Detect every UTF-8 whitespace sequence recognized by JavaScript `String.prototype.trim()` (ASCII whitespace `0x09..0x0D`, `0x20`, plus `U+00A0`, `U+1680`, `U+2000..U+200A`, `U+2028`, `U+2029`, `U+202F`, `U+205F`, `U+3000`, `U+FEFF`).
- **`P Delta 4 trim_and_count_body(V0, V1) => (R0, R1, R2)`**: Uses Zuse's `W` conditional loops to strip leading and trailing JS whitespace from `POST /posts` bodies and counts Unicode codepoints (UTF-8 leading bytes) via `W1(R2) => Z5`, returning `0` (valid `1..500` chars), `1` (empty after trim), or `2` (`> 500` chars).
- **`P Delta 5 verify_jwt_claims(...) => (R0, R1)`**: Verifies HMAC-SHA256 validity, enforces that both `sub` and `exp` are present (`SPEC.md` requires `exp`), checks `exp > now_s`, and parses `sub` as a positive user ID.
- **`P Delta 6 route_http_request(...) => (R0, R1, R2)`**: Combines `TABLE route_policy` with non-negative `Content-Length` enforcement on `POST` requests.
- **`P Delta 7 batch_pause_ns(V0, V1, V2) => R0`**: Computes the remaining nanosecond pause window (`2 ms` up to `32` queued writes) for adaptive write group-commit batching.

### 3. High-Throughput Runtime Bridge (`src/runtime.c`)

- **Share-Nothing Thread-Per-Core `epoll` + `SO_REUSEPORT` + `TCP_DEFER_ACCEPT`**: Spawns one pinned worker thread per available CPU core (`sched_getaffinity`), each owning its own `epoll` instance, its own `SO_REUSEPORT` + `TCP_DEFER_ACCEPT` listen sockets on `PORT` and `EXTRA_PORT`, and its own non-blocking `libpq` connections. Zero locks or cross-thread contention in the request path.
- **40-Byte Connection Slots (~40 MB RSS at 1,000,000 Connections)**: Idle keep-alive connections hold **zero** heap buffers (`in` and `out` buffers are returned immediately once a request/response completes).
- **Pipelined Binary `libpq` with Multi-Pass Socket Draining (`pg_read`) & Client-Only Batch Coalescing**:
  - Read connections stay permanently in `PQenterPipelineMode()` and send up to 64 `PQsendQueryPrepared` queries (binary results, `resultFormat = 1`) followed by a single `PQsendPipelineSync()`, amortizing Postgres network flushes and `Sync` messages across up to 64 concurrent requests.
  - `pg_read()` loops `PQconsumeInput()` -> `PQgetResult()` until no complete tuples remain in the kernel socket receive buffer, preventing large 160–320 KB `/feed` pipeline batches from stranding across `epoll_wait` cycles.
  - Plankalkül `P Delta 7` (`batch_pause_ns`) receives `client_evs` (excluding `TAG_PG` Postgres socket wakeups), so incoming database results are never stalled by batch pauses.
  - Write connections (`POST /posts`, `POST /posts/:id/like`) execute in isolated pipeline sync units (`PQsendPipelineSync()`) and wait for `PGRES_PIPELINE_SYNC` before sending any `201` / `200` HTTP response, guaranteeing every write is durably committed on Postgres before responding (Rule 8).
  - Total database connections: **up to 128** across all workers via 2D `zuse_pool_split` (capped at 16 per thread, well below the 512 limit).
- **Hardware-Accelerated Gzip (`Intel ISA-L igzip` Level 0 + Live Huffman Table Retraining)**: Compresses `GET /feed` responses (`>= 1024` bytes) in a single stateless pass using `isal_deflate_stateless` at level 0 with a per-thread `struct isal_hufftables` retrained once per second from the response byte histogram (`isal_update_histogram` + `isal_create_hufftables`, zero response/data caching), cutting `/feed` gzip size by ~15%.
