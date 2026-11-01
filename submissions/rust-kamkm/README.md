# Rust feed server

A Rust 1.94.0 submission using Tokio, httparse, tokio-postgres, and serde_json. Both configured ports share one process. Idle HTTP connections retain no request or response buffers. Parsing and handler futures are allocated while a request is active. The transport handles partial requests, HTTP/1.1 pipelining, Content-Length and chunked bodies, and gzip responses. It rejects missing or duplicate Host headers on HTTP/1.1, conflicting framing, repeated transfer encodings, and signed length fields. Idle connections have a 180-second timeout; the 15-second request timeout starts when actual data arrives.

A bounded queue feeds eight concurrent read batches of up to 64 requests. Each request executes its own prepared SELECT from `SPEC.md`. Concurrent polling lets tokio-postgres pipeline the statements. Response data and JWT verification results are never cached across requests.

Authenticated writes enter bounded batches of up to 64 jobs. Collection stops one millisecond after the first job. The driver pipelines the statements, commits the transaction, and then releases responses. Failed groups roll back and release the pooled connection before individual retries. HS256 signature and expiration checks run on every authenticated request. Unknown nested like paths return 404 before authentication; matched like routes still authenticate before validating the id. A missing token user produces an internal error rather than being misreported as a missing post.

The PostgreSQL pool defaults to 128 connections and accepts `DB_POOL` values from 1 through 512. Connections use `jit=off` and `plan_cache_mode=force_generic_plan`. Database waits and read batches have five-second bounds. Responses use UTC millisecond timestamps. Bodies of at least 1,024 bytes use gzip when accepted. The zlib-rs backend reuses a compressor on each executor thread; every response is an independent gzip member.

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

See [BENCHMARKS.md](BENCHMARKS.md) for validation and performance measurements. [validation-results.json](validation-results.json) records the current test matrix and source/executable hashes. [benchmark-results.json](benchmark-results.json) preserves the earlier development measurements at their recorded revision.

Front: direct
