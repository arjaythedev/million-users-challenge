# Go feed server

A Go 1.27.1 submission using gnet 2.10.0 and pgx 5.11.0. One process serves both ports, with one epoll event loop per `GOMAXPROCS` by default. Idle connections keep a socket record and timeout, without a goroutine or HTTP buffers. Only active requests start a goroutine. A fast parser handles ordinary HTTP/1.1 requests; Go's standard HTTP parser handles chunked bodies, Expect, encoded targets, and other request shapes. Pipelined requests execute in order. Idle connections have a 180-second timeout; a partial request has 15 seconds from its first byte.

Sixteen read workers send batches of up to 64 independent SELECT statements through pgx. Each response is delivered as soon as its own rows are read. The worker writes JSON directly from PostgreSQL's binary row values, with a typed-scan fallback. Every request executes the SQL from `SPEC.md`; response data and JWT verification results are never cached across requests. The read queue holds at most 4,096 jobs.

Authenticated writes enter four queues, each holding up to 256 pending jobs. Each writer pipelines groups of up to 64 statements in one implicit transaction. Likes for the same user/post pair use the same writer to preserve duplicate-like behavior. Writers reserve connections when the pool has space for reads. Responses wait for `ReadyForQuery`, after commit. A PostgreSQL statement error aborts its group before individual retries. A lost connection or timeout can leave the commit outcome uncertain, so those writes return errors without replay. With `DB_POOL=1`, writers release the connection between groups so reads can proceed.

The application checks each token's HS256 signature, expiration, and payload. Pooled HMAC states retain the key schedule, never token verification results. JSON encoding preserves the API's key order, escapes strings, and truncates UTC timestamps to milliseconds. Gzip writers and response buffers are pooled during active responses; bodies smaller than 1,024 bytes are sent directly. Idle timers move their deadlines without a reset on every complete request.

## Build and run

`install.sh` installs Go 1.27.1 on Ubuntu. `build.sh` pins that toolchain, downloads the checked-in module versions, and emits `bin/feed-go-kamkm` with cgo disabled. `start.sh` runs it in the foreground and defaults `GOMEMLIMIT` to a fixed `24GiB`. An explicit environment value takes precedence. The script does not inspect `/proc/meminfo`.

Set `DATABASE_URL`, `JWT_SECRET`, `HOST`, `PORT`, and optional `EXTRA_PORT`. `DB_POOL` defaults to 128 and accepts 1 through 512. Connections use `jit=off` and `plan_cache_mode=force_generic_plan`. Each database batch has a five-second timeout.

Optional process controls are `HTTP_LOOPS` (default `GOMAXPROCS`, 1..256), `READ_WORKERS` (default 16, reduced for small pools, 1..512), `READ_BATCH` (default 64, 1..256), and `WRITE_LANES` (default up to 4, reduced for small pools, 1..64). `DB_POOL` remains the total connection ceiling.

## Validation

Run the repository API suite against a fresh seed clone, then the regressions:

```sh
bash test/run.sh submissions/go-kamkm
cd submissions/go-kamkm && go test -race . && go vet .
# With the server running:
python3 tests/feed_freshness.py http://127.0.0.1:3000 --database feed_test
python3 tests/concurrent_writes.py http://127.0.0.1:3000 --database feed_test
python3 tests/http_transport.py http://127.0.0.1:3000 --idle 16
```

The database probes use the usual `psql` connection environment, or `--docker CONTAINER`. Unit regressions compare the fast parser with `net/http`, check JSON and timestamps against the standard library, reject malformed framing, and require a string token username.

## Measurements

See [BENCHMARKS.md](BENCHMARKS.md) for the current optimization measurements and earlier connection tests. [optimization-results.json](optimization-results.json) records the follow-up runs; [benchmark-results.json](benchmark-results.json) preserves the earlier measurements. These development results are not official leaderboard scores.

Front: direct
