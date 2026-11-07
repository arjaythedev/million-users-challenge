# Rust feed server

A Rust 1.94.0 submission using Tokio, httparse and tokio-postgres, with mimalloc as the global allocator. Both configured ports share one process. Idle HTTP connections retain no request or response buffers: a connection waits for readiness without a buffer, and a request's buffer is allocated with its first read, so there is no separate `peek` per request. Parsing and handler futures are allocated while a request is active. The transport copies only the method, path, first Authorization header and body out of the read buffer, and writes each response's head and body with one write. It handles partial requests, HTTP/1.1 pipelining, Content-Length and chunked bodies, and gzip responses. It rejects missing or duplicate Host headers on HTTP/1.1, conflicting framing, repeated transfer encodings, and signed length fields. Idle connections have a 180-second timeout; the 15-second request timeout starts when actual data arrives.

A bounded queue feeds up to 48 concurrent read batches (`READ_BATCHES`, 1..512, at most `DB_POOL`) of up to 64 requests. Each request executes its own prepared SELECT from `SPEC.md`. Concurrent polling lets tokio-postgres pipeline the statements. A batch holds its slot until all of its results are in, so too few slots leave requests queued while the database is idle. Each handler writes its JSON directly from the row values: text columns are read in place, timestamps are formatted from PostgreSQL's microseconds, and strings are escaped exactly as serde_json escapes them. Response data and JWT verification results are never cached across requests.

Authenticated writes enter bounded batches of up to 64 jobs, with up to four batches in flight (`WRITE_BATCHES`, 1..512, at most `DB_POOL`); the next batch collects while earlier ones commit. Collection stops one millisecond after the first job. A user sends one request at a time, so concurrent batches do not hold the same like; a duplicate like that does reach two batches waits for the first to commit and reports `already_liked`. The driver pipelines the statements, commits the transaction, and then releases responses. Failed groups roll back and release the pooled connection before individual retries. HS256 signature and expiration checks run on every authenticated request; only the decoding key and validation rules are built once. Unknown nested like paths return 404 before authentication; matched like routes still authenticate before validating the id. A missing token user produces an internal error rather than being misreported as a missing post.

The PostgreSQL pool defaults to 128 connections and accepts `DB_POOL` values from 1 through 512. Connections use `jit=off` and `plan_cache_mode=force_generic_plan`. Database waits and read batches have five-second bounds. Responses use UTC millisecond timestamps. Bodies of at least 1,024 bytes use gzip when accepted, from a small built-in encoder (`src/fast.rs`): one DEFLATE block with RFC 1951's fixed Huffman codes and greedy LZ77 matches from a single-probe hash table, with a CRC-32 trailer. For few-KiB JSON bodies a general-purpose compressor spends much of its time building Huffman tables per response; the fixed codes need none. Every response is an independent gzip member, and unit tests decode the output with flate2 for every match length and distance code, window edges and random input.

## Build and run

```sh
./install.sh
./build.sh
DATABASE_URL=postgres://feed:password@db:5432/feed \
JWT_SECRET=million-users-challenge HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 ./start.sh
```

The locked release build uses Rust 1.94.0, fat LTO, and the build machine's CPU. To build locally for another x86-64 server, use `RUSTFLAGS='-C target-cpu=x86-64-v2' ./build.sh`. The database driver uses plain TCP on the private challenge network.

## Validation

Run the repository API suite against a fresh seed clone, then the regressions:

```sh
bash test/test.sh http://127.0.0.1:3000
python3 submissions/rust-kamkm/tests/feed_freshness.py http://127.0.0.1:3000 --database feed_test
python3 submissions/rust-kamkm/tests/concurrent_writes.py http://127.0.0.1:3000 --database feed_test
python3 submissions/rust-kamkm/tests/http_transport.py http://127.0.0.1:3000 --idle 16
python3 submissions/rust-kamkm/tests/contract_edges.py http://127.0.0.1:3000
```

The database probes use the usual `psql` connection environment, or `--docker CONTAINER`. Repeat the API and regression suites with `DB_POOL=1`.

## Measurements

See [BENCHMARKS.md](BENCHMARKS.md) for validation and performance measurements. [validation-results.json](validation-results.json) records the test matrix and source/executable hashes at `eedc22d`. [benchmark-results.json](benchmark-results.json) preserves the earlier development measurements at their recorded revision.

Front: direct
