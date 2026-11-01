# Zig feed server

A Linux x86-64 submission built with Zig 0.13.0. Two `SO_REUSEPORT` epoll loops share a total budget of 16 database workers. Idle sockets retain a small connection record. Active request buffers start at 512 bytes and double as needed up to the 16-KiB request limit. The server releases each buffer once all buffered requests have been served. It preserves ordered HTTP/1.1 pipelining, accepts chunked bodies and HTTP continue, and honors connection closure. Malformed or ambiguous framing receives a 400 response and closes the connection.

Each worker owns one libpq connection and prepares the four SQL statements from `SPEC.md`. Read workers send up to 64 queued SELECT statements before a shared pipeline Sync. Each request still executes its own SQL. Two write workers pipeline up to 64 writes in a transaction and hold responses until commit. Failed statements roll back before individual retries. An uncertain COMMIT result returns an error without replaying the writes. JWTs are verified on every authenticated request with OpenSSL HMAC-SHA256. Response data is never cached across requests.

ISA-L 2.31.1 compresses responses of at least 1,024 bytes when the client accepts gzip. Compression state and scratch buffers belong to the worker thread. Short responses avoid compression overhead. Post bodies must be valid UTF-8; Unicode whitespace is trimmed before the 500-character limit is checked. Embedded NUL bytes are rejected without truncating a database value.

## Build and run

`install.sh` installs Zig and the C build tools on Ubuntu. `build.sh` downloads SHA-256-pinned PostgreSQL 18.6 and ISA-L sources, links their static client libraries, and emits `feed-zig`. The PostgreSQL client uses plain TCP on the challenge's private database network. OpenSSL supplies JWT verification. The installer reuses an existing pinned compiler, and the build rejects a different Zig version.

Set `DATABASE_URL`, `JWT_SECRET`, `HOST`, `PORT`, and optional `EXTRA_PORT`, then run `./start.sh` in the foreground.

`DB_POOL` sets the total database connection budget, from 1 through 512. `HTTP_LOOPS` sets the number of epoll loops, defaulting to two when the pool has at least four workers. Each loop needs a read worker. `WRITE_WORKERS` sets the total reserved write workers; use zero to disable reservation, or at least one per loop. Defaults are 16 database workers, two loops, and two write workers.

## Validation

Run the repository API suite against a fresh seed clone, then the regressions:

```sh
bash test/test.sh http://127.0.0.1:3000
python3 submissions/zig-kamkm/tests/request_buffers.py http://127.0.0.1:3000
python3 submissions/zig-kamkm/tests/feed_freshness.py http://127.0.0.1:3000 --database feed_test
python3 submissions/zig-kamkm/tests/concurrent_writes.py http://127.0.0.1:3000 --database feed_test
python3 submissions/zig-kamkm/tests/contract_edges.py http://127.0.0.1:3000
python3 submissions/zig-kamkm/tests/http_transport.py http://127.0.0.1:3000 --idle 16
```

The Python probes use the usual `psql` connection environment. Add `--docker CONTAINER` when PostgreSQL runs in Docker. Repeat the suite with `DB_POOL=1`.

To exercise partial sends and retryable send failures, compile `tests/send_faults.c` with `cc -shared -fPIC -std=c11 -Wall -Wextra -Werror tests/send_faults.c -ldl -o /tmp/zig-send-faults.so`. Start a test server with `LD_PRELOAD=/tmp/zig-send-faults.so` and rerun the Python suites. The shim limits nonblocking sends and returns `EAGAIN` on every seventh call.

## Measurements

See [BENCHMARKS.md](BENCHMARKS.md) for validation and performance measurements. [validation-results.json](validation-results.json) records the current test matrix, load results, and source/executable hashes. [benchmark-results.json](benchmark-results.json) preserves the earlier development measurements at their recorded revision.

Front: direct
