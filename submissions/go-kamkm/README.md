# Go feed server

A Go 1.27.1 submission using gnet 2.10.0 and pgx 5.11.0. Four epoll event loops serve both ports in one process. Idle connections keep a socket record and timeout, without a goroutine or HTTP buffers. Only active requests start a goroutine. Go's standard HTTP parser handles partial headers, chunked bodies, and pipelining. A connection processes pipelined requests in order. Idle connections have a 180-second timeout; a request has 15 seconds from its first byte.

Eight read workers send batches of up to 64 independent SELECT statements through pgx. Each request executes the SQL from `SPEC.md`; response data and JWT verification results are never cached across requests. The bounded read queue holds at most 1,024 jobs.

Authenticated writes enter a bounded queue and share transaction commits in groups of up to 64. The writer reserves a database connection to avoid waiting behind read traffic, pipelines the statements, and releases responses after commit. Failed statements roll back before individual retries. An uncertain COMMIT result returns an error without replaying the writes. With `DB_POOL=1`, the writer releases the connection between batches so reads can proceed. Shutdown stops the writer and releases its connection before closing the pool.

The application validates HS256 signatures, expiration, and token payloads on every authenticated request. Ordered response structs preserve the API's JSON format. Gzip writers and output buffers are pooled during active responses; bodies smaller than 1,024 bytes are sent directly.

## Build and run

`install.sh` installs Go 1.27.1 on Ubuntu. `build.sh` pins that toolchain, downloads the checked-in module versions, and emits `bin/feed-go-kamkm` with cgo disabled. `start.sh` runs it in the foreground and supplies a 75% physical-memory `GOMEMLIMIT` when none is set.

Set `DATABASE_URL`, `JWT_SECRET`, `HOST`, `PORT`, and optional `EXTRA_PORT`. `DB_POOL` defaults to 128 and accepts 1 through 512. Connections use `jit=off` and `plan_cache_mode=force_generic_plan`. Database operations have a five-second timeout.

## Validation

Run the repository API suite against a fresh seed clone, then the regressions:

```sh
bash test/test.sh http://127.0.0.1:3000
python3 submissions/go-kamkm/tests/feed_freshness.py http://127.0.0.1:3000 --database feed_test
python3 submissions/go-kamkm/tests/concurrent_writes.py http://127.0.0.1:3000 --database feed_test
python3 submissions/go-kamkm/tests/http_transport.py http://127.0.0.1:3000 --idle 16
```

The database probes use the usual `psql` connection environment, or `--docker CONTAINER`.

## Measurements

See [BENCHMARKS.md](BENCHMARKS.md) for the remote before/after tests, the matched C comparison, and the sustained connection test. [benchmark-results.json](benchmark-results.json) records the measured metrics and executable hashes.

Front: direct
