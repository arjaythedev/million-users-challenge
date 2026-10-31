# OCaml 5.5 + httpun + eio

An implementation of the Feed API in OCaml Multicore, using the
[httpun](https://github.com/anmonteiro/httpun) high-performance webserver with
the [eio](https://github.com/ocaml-multicore/eio) scheduler and a pipelined
Postgres client.

The Eio scheduler provides both native `io_uring` support on recent kernels
(with graceful degradation to syscalls when unavailable) and multicore fibers
(also called "green threads" in other languages), such that we get to write
simple code with fast I/O.

We reuse the existing `httpun` webserver and drive it with a single fiber per
connection, so that the code is both performant and natural (as if we had one
thread per client). For Postgres, we add a tiny (incomplete) library to support
the wire protocol v3 with batching and pipelining of requests. We use the
existing [`jsont`](https://github.com/dbuenzli/jsont) library for fast JSON
decoding, and add a custom `jsonbuf` helper library to optimize JSON rendering
for the known API response schemas. We compress HTTP responses only when they
are large enough to be worth it, using custom bindings to `libdeflate`.
Finally, after profiling, we use OCaml Multicore to share pure CPU
computations across cores: rendering rows to JSON and gzipping bytes are
memoized. This is not a data cache: every request still runs its own query
(see [below](#how-it-works)).

| | |
|---|---|
| Language | OCaml 5.5.0 |
| Scheduler | Eio 1.6 (`eio_linux` for `io_uring`) |
| HTTP | httpun 0.2.0 (parser and serializer), driven by a custom lightweight connection loop |
| Postgres | custom client (`lib/pgwire`): wire protocol v3, pipelined, SCRAM-SHA-256 |
| JSON | encoders built once, writing into reused buffers (`lib/jsonbuf`); request bodies and JWTs decoded with jsont 0.4.0 |
| gzip | libdeflate (Ubuntu 24.04's `libdeflate-dev`), level 1 |
| JWT | our own HS256 check (`lib/jwt`), digestif 1.4.1, eqaf 0.10 |

## How it works

- **One Eio domain per CPU** (configured by `DOMAINS`, defaults to `nproc`):
  In OCaml, a "domain" is a heavy-weight thread which runs on one core.
  Each domain has its own listening sockets on both ports (`SO_REUSEPORT`), its own
  accept loop and its own Postgres pool.

- **One fiber per connection:** we drive httpun's state machine directly, using a
  helper library in `lib/httpun_fiber`: write while there is output, else read.
  (`httpun-eio`'s general-purpose handlers use several fibers per connection
  and fresh fibers per request, so this custom loop was 2.7× cheaper for the
  use case of API requests over long-lived connections.)

- **Postgres batching**: every query is Bind/Execute on a statement prepared
  when the connection opens. Queries fill one connection's batch, sent in one
  `write` once it holds 1,400 bytes or 2 ms after its first query; consecutive
  reads share one Sync (one round trip, one implicit transaction), each write
  gets its own Sync and is answered only after its commit. The next batch goes to
  the next idle connection, round-robin; when none is idle the pool opens one
  more. Answers are read into a 128 KB buffer, and each query completes as soon
  as its own answer is in. Reads and writes use separate connections (such that
  reads never wait behind a commit). Each domain starts with 2 + 2 connections,
  within a total budget of 500. Batching cuts our CPU per request by up to ~25%
  and Postgres's by ~7–19% below saturation, for ~2 ms more latency at low load.

- **Small per-connection buffers**: about 7 KB in total per open connection.

- **Few allocations on the major heap per request**: JSON bodies are built in a
  per-domain buffer and compressed through per-domain byte arrays.

- **gzip** only for bodies of at least 1 KB, when `Accept-Encoding` includes
  gzip.

- **Memoizing pure functions** (`lib/memo`): rendering rows to JSON and
  gzipping bytes are pure functions. We memoize them in a 4,096-slot table
  shared by all domains, lock-free with one `Atomic` per slot (the last writer
  wins). The key is the complete row data returned by *this* request's query,
  compared by full equality (not just its hash), so a hit gives exactly the
  bytes we would have computed. Every request still runs its query and reads
  its data from Postgres (rule 7); only the CPU work of turning identical rows
  into identical bytes is skipped. This is the only state shared across cores,
  and shows what native multicore support in a high-level language like OCaml
  makes easy once profiling reveals a need for it.

- **GC**: `space_overhead=400` and a 16 MB minor heap (unless `OCAMLRUNPARAM`
  is set): light GC tuning for heavy load.

- **JWT** (`lib/jwt`): on every request, the HS256 signature is checked with a
  constant-time compare (eqaf), then `alg` and `exp`. Nothing is remembered
  between requests.

## Build

`install.sh` installs a C toolchain, opam and libdeflate (running needs
`libdeflate0`, pulled in by `libdeflate-dev`). `build.sh` creates a local opam
switch with every package at the version in `feed-api.opam.locked`, from
opam-repository pinned to one commit.

## Configuration

Supports `DATABASE_URL`, `JWT_SECRET`, `HOST` (default `0.0.0.0`), `PORT`
(default 3000) and `EXTRA_PORT`, as required by the spec.

Optional:

| Variable | Default |
|---|---|
| `DOMAINS` | `nproc` |
| `DB_POOL` / `DB_WRITE_POOL` | 2 / 2 connections per domain at start |
| `DB_POOL_MAX` | min(24, ⅔ · 500 / domains) per domain |
| `DB_WRITE_POOL_MAX` | min(12, ⅓ · 500 / domains) per domain |
| `DB_BATCH_BYTES` | 1400 |
| `DB_BATCH_WAIT_US` | 2000 |
