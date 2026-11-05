# Rust feed server: one thread per core on raw io_uring

A Rust 1.94.0 submission with no async runtime and no database driver: each thread runs its own
io_uring ring (`SINGLE_ISSUER`, `DEFER_TASKRUN`), its own `SO_REUSEPORT` listeners on `PORT` and
`EXTRA_PORT`, its own slice of the Postgres connections (16 read + 32 write per process, spoken
to with the ~400-line wire client in `src/pg.rs`) and its own ISA-L compressor. Dependencies are
five pinned crates: `io-uring`, `libc`, `hmac`+`sha2` (HS256 and SCRAM), `memchr`, and ISA-L
built from its SHA-256-pinned tarball in `build.sh`.

## Where the requests go

- **Accept/receive**: one multishot accept per listener places sockets directly into the ring's
  registered file table; each connection gets one multishot receive drawing from a ring of 4,096
  provided 2 KB buffers. An idle connection owns nothing — 16 bytes of state in a table indexed
  by slot, with a generation counter to ignore stale completions. Requests are parsed in place
  and the buffer handed straight back.
- **One wait per batch**: after a pass that did work, a single `io_uring_enter` submits every
  queued send/receive and waits for 128 completions or 2 ms. Requests that arrive in that window
  share one wake-up — and their database reads share one Postgres round trip.
- **Reads share a Sync**: reads queue on the first live read connection with fewer than 64 in
  flight; `pg_flush` closes each pass's pack with one `Sync`, so Postgres executes it as one
  pipeline with one flush. Every read is answered at its own CommandComplete. Writes get their
  own connection and their own Sync (one transaction each), and the response goes out only after
  ReadyForQuery — committed, with no replay of uncertain commits on connection loss.
- **Rows become JSON in place**: DataRows are read as binary (int8/int4/timestamptz/text) and
  serialized directly into the response buffer; timestamps are formatted from raw microseconds.
  No intermediate representation, no serde, no allocations in the hot path beyond the response
  buffer, which is pooled.
- **gzip**: ISA-L igzip, stateless, level 0, only for bodies of 1 KB or more when accepted.
  Failure to compress (never happens with the sized bound, but is handled) falls back to plain.
- **Failure behavior**: a lost Postgres connection fails only its queued requests (500; health
  503 `degraded`), reconnects one per pass with a 1 s backoff, and TCP keepalive with a 60 s user
  timeout turns a silent peer into an error instead of a hang.

## HTTP semantics carried over from the earlier revision

The transport keeps the hardening this submission was already validated with, now hand-written
(no httparse): `Content-Length` must be unsigned digits (conflicting duplicates rejected,
identical ones allowed), `Transfer-Encoding` must be exactly `chunked`, the two may not be
combined, duplicate/missing `Host` on HTTP/1.1 is rejected, control bytes in the target or
header values are rejected. Chunked bodies are decoded (trailers included) so pipelined requests
behind them are answered; partial requests, pipelining, `Expect: 100-continue` and HTTP/1.0
keep-alive work as before. Framing violations answer 400 and close.

JWT: HS256 only, signature and `exp` (zero leeway) verified on every request with a pre-keyed
HMAC (the clone has the key schedule done); `sub` must be a positive integer string and
`username` a string. A like's missing post is recognized by its constraint
(`likes_post_id_fkey`), so a token naming a nonexistent user stays a 500 instead of a 404.

## Build and run

```sh
./install.sh   # build-essential, curl, nasm, Rust 1.94.0
./build.sh     # ISA-L 2.31.1 (pinned) + cargo build --release --locked, -C target-cpu=native
DATABASE_URL=postgres://feed:password@db:5432/feed \
JWT_SECRET=million-users-challenge HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 ./start.sh
```

Needs Linux 6.1+ with io_uring enabled (Ubuntu 24.04 is 6.8). Rule 13 means no kernel tuning
here; the loop is built to work with what `box/setup-app-box.sh` provides.

## Validation

Run the repository API suite against a fresh seed clone (as `test/run.sh` does in CI), then the
regressions:

```sh
bash test/test.sh http://127.0.0.1:3000
python3 submissions/rust-kamkm/tests/feed_freshness.py http://127.0.0.1:3000 --database feed_test
python3 submissions/rust-kamkm/tests/concurrent_writes.py http://127.0.0.1:3000 --database feed_test
python3 submissions/rust-kamkm/tests/http_transport.py http://127.0.0.1:3000 --idle 16
python3 submissions/rust-kamkm/tests/contract_edges.py http://127.0.0.1:3000
```

`cargo test` covers URL/base64 parsing, DataRow extraction, timestamps, id/body validation and
the framing matrix without a server. [BENCHMARKS.md](BENCHMARKS.md) records the earlier
revision's results and the rewrite's development measurements.
