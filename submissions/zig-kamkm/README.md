# Zig feed server

A Linux x86-64 submission built with Zig 0.13.0. Two `SO_REUSEPORT` epoll loops share a total budget of 16 database workers. Idle sockets retain a small connection record. Active request buffers start at 512 bytes and double as needed up to the 16-KiB request limit. The server releases each buffer after sending the response.

Each worker owns one libpq connection and prepares the four SQL statements from `SPEC.md`. Read workers send up to 64 queued SELECT statements before a shared pipeline Sync. Each request still executes its own SQL. Two write workers pipeline up to 64 writes in a transaction and hold responses until commit. Failed statements roll back before individual retries. An uncertain COMMIT result returns an error without replaying the writes. JWTs are verified on every authenticated request with OpenSSL HMAC-SHA256. Response data is never cached across requests.

ISA-L 2.31.1 compresses responses of at least 1,024 bytes when the client accepts gzip. Compression state and scratch buffers belong to the worker thread. Short responses avoid compression overhead.

## Build and run

`install.sh` installs Zig and the C build tools on Ubuntu. `build.sh` downloads SHA-256-pinned PostgreSQL 18.6 and ISA-L sources, links their static client libraries, and emits `feed-zig`. The PostgreSQL client uses plain TCP on the challenge's private database network. OpenSSL supplies JWT verification.

Set `DATABASE_URL`, `JWT_SECRET`, `HOST`, `PORT`, and optional `EXTRA_PORT`, then run `./start.sh` in the foreground.

`DB_POOL` sets the total database connection budget, from 1 through 512. `HTTP_LOOPS` sets the number of epoll loops, defaulting to two when the pool has at least four workers. Each loop needs a read worker. `WRITE_WORKERS` sets the total reserved write workers; use zero to disable reservation, or at least one per loop. Defaults are 16 database workers, two loops, and two write workers.

## Validation

Run the repository API suite against a fresh seed clone, then the regressions:

```sh
bash test/test.sh http://127.0.0.1:3000
python3 submissions/zig-kamkm/tests/request_buffers.py http://127.0.0.1:3000
python3 submissions/zig-kamkm/tests/feed_freshness.py http://127.0.0.1:3000 --database feed_test
python3 submissions/zig-kamkm/tests/concurrent_writes.py http://127.0.0.1:3000 --database feed_test
```

The Python probes use the usual `psql` connection environment. Add `--docker CONTAINER` when PostgreSQL runs in Docker.

## Measurements

See [BENCHMARKS.md](BENCHMARKS.md) for measured throughput, latency, and sustained connection tests. [benchmark-results.json](benchmark-results.json) records the measured metrics and executable hashes.

Front: direct
