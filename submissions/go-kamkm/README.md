# Go feed server

A Go 1.27.1 submission using gnet 2.10.0 and pgx 5.11.0. One process serves both ports, with one epoll event loop per `GOMAXPROCS` by default. Idle connections keep only a small socket record, without a goroutine, a runtime timer or HTTP buffers. No goroutine is started per request either: the event loop parses a request and queues it for a read pipeline or write lane, and a fixed pool of responders (one per event loop) encodes, compresses and sends each completed response. A fast parser handles ordinary HTTP/1.1 requests; Go's standard HTTP parser handles chunked bodies, Expect, encoded targets, and other request shapes. Pipelined requests execute in order. Idle connections have a 180-second timeout; a partial request has 15 seconds from its first byte. Deadlines are enforced by one timing wheel with one-second slots, swept by a single ticker, instead of a runtime timer per connection: with a million connections, per-connection timers make every scheduler pass that adjusts the timer heap walk all of them, so idle connections cost CPU in proportion to their number. Requests only move a connection's deadline; a sweep re-files a connection whose deadline moved, so an active connection is examined about once per idle timeout.

Forty-eight read workers send batches of up to 64 independent SELECT statements through pgx. Each response is delivered as soon as its own rows are read. The worker writes JSON directly from PostgreSQL's binary row values, with a typed-scan fallback. Every request executes the SQL from `SPEC.md`; response data and JWT verification results are never cached across requests. The read queue holds at most 65,536 jobs; when it is full the event loops stop taking new requests until the pipelines catch up.

Authenticated writes are verified on the event loop, then enter four queues, each holding up to 4,096 pending jobs. Each writer pipelines groups of up to 64 statements in one implicit transaction. Likes for the same user/post pair use the same writer to preserve duplicate-like behavior. Writers reserve connections when the pool has space for reads. Responses wait for `ReadyForQuery`, after commit. A PostgreSQL statement error aborts its group before individual retries. A lost connection or timeout can leave the commit outcome uncertain, so those writes return errors without replay. With `DB_POOL=1`, writers release the connection between groups so reads can proceed.

The application checks each token's HS256 signature, expiration, and payload. Pooled HMAC states retain the key schedule, never token verification results. JSON encoding preserves the API's key order, escapes strings, and truncates UTC timestamps to milliseconds. Bodies of 1,024 bytes or more are gzipped when the client accepts it, by a small built-in encoder (`gzipfast.go`): one DEFLATE block with the fixed Huffman codes of RFC 1951, greedy LZ77 matches from a single-probe hash table, and the standard CRC-32 trailer. For few-KiB JSON bodies a general-purpose compressor spends most of its time building Huffman tables per response; the fixed codes need none, which halves the cost of a feed response (about 8 µs instead of 16 µs for klauspost/compress level 1) for about 17% more bytes. Smaller bodies are sent directly. Response buffers are pooled.

Requests waiting for database admission retain no response payload buffers. A queued request holds only its parsed fields. A read worker acquires its JSON buffer only after receiving its query result, then transfers ownership to a responder. Writes acquire their JSON buffer after commit, and HTTP wire buffers are acquired after the response body is ready. This prevents a slow database from turning queued handlers into several gigabytes of unused 4-KiB buffers.

## Build and run

`install.sh` installs Go 1.27.1 on Ubuntu. `build.sh` pins that toolchain, downloads the checked-in module versions, and emits `bin/feed-go-kamkm` with cgo disabled. `start.sh` runs it in the foreground and defaults `GOMEMLIMIT` to a fixed `24GiB` and `GOGC` to 400: the process stayed at about 3 GB resident with a million connections in development runs under `GOMEMLIMIT=4GiB`, and each collection scans every connection's state, so collecting less often saves CPU while `GOMEMLIMIT` still bounds the heap. Explicit environment values take precedence. The script does not inspect `/proc/meminfo`.

Set `DATABASE_URL`, `JWT_SECRET`, `HOST`, `PORT`, and optional `EXTRA_PORT`. `DB_POOL` defaults to 128 and accepts 1 through 512. Connections use `jit=off` and `plan_cache_mode=force_generic_plan`. Each database batch has a five-second timeout.

Optional process controls are `HTTP_LOOPS` (default `GOMAXPROCS`, 1..256), `READ_WORKERS` (default 48, reduced for small pools, 1..512), `READ_BATCH` (default 64, 1..256), and `WRITE_LANES` (default up to 4, reduced for small pools, 1..64). `DB_POOL` remains the total connection ceiling.

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
