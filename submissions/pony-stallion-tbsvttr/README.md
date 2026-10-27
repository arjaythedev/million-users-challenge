# pony-stallion-tbsvttr: a million users on a Pony

Pony is an actor language that compiles to native code. Its type system rules out data races at compile time, and
every actor has its own heap and its own garbage collector. This is the feed API (`SPEC.md`) in Pony: stallion for
HTTP (one actor per connection), the pure-Pony postgres driver, and the standard library for JSON, HMAC and base64.
Both libraries come from the ponylang org.

## Stack

| Piece | Version |
|---|---|
| Compiler | ponyc 0.77.1, the official release build for Ubuntu 24.04, SHA-512 pinned in `install.sh`, in `/opt/ponyc` |
| Dependency manager | corral 0.9.2, SHA-512 pinned, in `/opt/corral` |
| HTTP | stallion 0.16.0 |
| Postgres driver | postgres 0.15.1 (pure Pony, SCRAM-SHA-256, extended protocol) |
| JSON | stdlib `json` for the request body; response JSON is built by hand |
| JWT | hand-written HS256 check on stdlib `crypto` (`HmacSha256`, `ConstantTimeCompare`) and `encode/base64` |
| Compression | libdeflate (Ubuntu's `libdeflate-dev`) over FFI, gzip at level 1; the stdlib has no zlib |

`corral.json` and `lock.json` pin both libraries to their release tags. `build.sh` runs `corral fetch` and compiles
an optimized binary, `build/feed`, with `-Dopenssl_3.0.x` (Ubuntu 24.04's OpenSSL, which the stdlib's `crypto`
links). `start.sh` runs it in the foreground.

## Process model

- **One process.** The Pony runtime starts one scheduler thread per core and runs actors on them, work-stealing.
- **One actor per connection** (`Conn`, a stallion `HTTPServerActor`). Each port has its own `TCPListener`; both
  are in the same process. The listener's default cap of 100,000 connections is lifted (`limit = None`).
- **Idle timeout 120 s** (stallion's default is 60 s, below the spec's 75 s).
- **Read buffer 1 KB per connection** (default 16 KB). Every request here fits; a bigger one is read in pieces.

## Database

- **256 `DbSlot` actors, one Postgres connection each** (`DB_POOL` sets the count). The driver runs one query at a
  time per connection and queues the rest. Each `Conn` is pinned to one `DbSlot`, round-robin by accept order.
- Every statement is SPEC.md's SQL, prepared once per connection when it logs in, one per request, in autocommit.
  Queries that arrive while the connection is still logging in wait for it.
- **When Postgres goes away,** a `DbSlot` fails what is waiting and reconnects on the next query, at most once a
  second. `/health` answers 503 and everything else 500 until then.
- Like on a missing post: `foreign_key_violation` (`23503`) becomes 404 `post not found`.
- `created_at` is truncated (not rounded) to milliseconds.

## Memory per connection

The first version used ~296 KB per idle connection, so 125,000 users ran out of memory at ~43,000 connections. I
measured layer by layer (idle connections, RSS before and after):

| What | Per idle connection |
|---|---:|
| A bare `TCPConnection` actor | 19 KB |
| stallion's hello example | 23 KB |
| This server, first version | 296 KB |
| This server, now | **11 KB** |

**The cause was Pony's GC (ORCA).** An actor that receives a reference to another actor keeps a record of it for
the garbage collector. Each `Conn` got a `Context` that held the `Database`, which reaches all 256 `DbSlot` actors.
So every connection kept 256 actor records, ~290 KB. Now a `Conn` gets the JWT secret, the start time and its one
`DbSlot`. The 1 KB read buffer accounts for the rest of the gap to stallion's example.

Under `mload` with 125,000 connections, RSS was 1,678 MB, ~13 KB per connection. A million would need ~13 GB.

## Results

`test/run.sh`: **passed=42 failed=0** on Ubuntu 24.04 against `postgres:18`, both on aarch64 (Docker on an Apple
Silicon Mac) and on x86_64 (the same, emulated with Rosetta). `install.sh` installs and verifies both archives on
both.

Also checked by hand: `created_at` `.999999` comes out `.999Z`; gzip on the feed (1,840 bytes) unzips to the plain
body; a single post isn't gzipped; the full API on `EXTRA_PORT`; `Expect: 100-continue`; 200 requests over one
connection; 2,000 requests 200 at a time; a connection idle for 80 s still answers; 503/500 while the database is
unreachable, then recovery.

### Local load tests (not the benchmark hardware)

Same setup as `typescript-bun-tbsvttr` and `elixir-bandit-tbsvttr`: Docker on an Apple Silicon Mac, the server's
container pinned to N cores, Postgres and `mload` on their own cores. 125,000 users on 2 cores is the box's
million on 16 cores, scaled down.

| Server | Cores | Load | Result | Server CPU (hold) | Server RSS |
|---|---:|---|---|---:|---:|
| **Pony** | 1 | 10,000 users, `-gzip -think-scale 0.5` (~1,940 req/s) | p95 19 ms | 0.82 cores | 210 MB |
| **Pony** | 2 | the same | p95 17 ms | 1.66 cores | 215 MB |
| **Pony** | 2 | 125,000 users, `-gzip -think-scale 2.5`, 3 min hold | **fail**: p95 214 ms, p99 1.63 s, 0 errors | 1.80 cores | 1,678 MB |
| Elixir | 2 | the same | fail: p95 788 ms, p99 2.96 s, 0 errors | 1.38 cores | 3,972 MB |
| Bun | 2 | the same | pass: p95 3.9 ms | ~0.45 cores | 167 MB |
| Node example | 2 | the same | pass: p95 3.2 ms | 0.84 cores | 1,011 MB |

- **The CPU column overstates the work.** With the same load on 2 cores instead of 1, CPU went from 0.82 to 1.66
  cores: idle scheduler threads spin before they sleep. How much of the 1.80 cores at 125,000 users is real work, I
  couldn't separate without a profiler.
- **Latency:** p95 sits at ~17 ms even at light load, where Bun answers in ~1 ms. I haven't found the cause.
- **The 125,000 run** held every connection with no errors. Over most of the hold, p95 was 15–24 ms. The p99 failure
  comes from three bursts: at the end of the ramp, at ~160 s, and at ~200 s. The last one (p95 2.2 s for 10 s) is
  when `mload` collected its own garbage at its memory limit. Bun and Node passed with the same disturbance.
- **On the box:** memory is fine now (~13 GB for a million). CPU is the limit: at the scaled-down million the
  server already ran near its 2 cores locally, and a core on the box is slower than one here. My guess for 16 cores
  is 500,000–700,000 users.
