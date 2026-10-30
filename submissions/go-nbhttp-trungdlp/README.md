# Go 1.27 + nbhttp + pgx

Feed API submission for `trungdlp`, adapted from `examples/go`.

| Component | Version |
| --- | --- |
| Go | 1.27.3, CGO disabled |
| HTTP engine | github.com/lesismal/nbio/nbhttp v1.7.1 |
| Routing / JSON | custom `net/http.Handler` route switch / github.com/bytedance/sonic v1.15.5 |
| PostgreSQL | github.com/jackc/pgx/v5 v5.11.0, native `pgxpool` |
| JWT | custom HS256 verifier with Sonic claims decoding (`golang-jwt` is test-only) |
| Compression | github.com/klauspost/compress/gzhttp v1.20.3, gzip level 1 |

## Design

One process serves both ports through one nonblocking nbhttp engine. Idle connections
use nbio's pollers rather than a dedicated goroutine per connection. HTTP handlers run
in nbhttp's worker pool, so waiting on PostgreSQL does not block an I/O poller.
The handlers remain compatible with the standard `http.Handler` interface.
The five fixed routes use a small method/path switch instead of a general-purpose
router; path parsing is limited to the two post routes.

- Pollers: `max(1, GOMAXPROCS / 4)`, following nbhttp's default ratio while respecting
  the runtime's CPU limit. Handler worker limit: 1024. These are starting values,
  not settings established by a full benchmark.
- Connection limit: 4,000,000 across both ports. The nbio fd table is raised from its
  default 2M entries to 4,194,304 entries to cover the challenge's 3M-user search.
  This changes process memory only; the host must supply the specified file limit.
- HTTP keep-alive: 120 seconds. TCP keep-alive probes are disabled on listeners.
- PostgreSQL pool: `min(128, 8 * GOMAXPROCS)`, shared by all handlers and ports;
  the application overrides `pool_max_conns` from the connection string.
- Reference SQL, one statement per request, with pgx's default prepared statement
  cache. No response, query-result or JWT verification caching. Writes use autocommit
  and complete before the response is written.
- Sonic `ConfigFastest` encodes response structs; `ConfigStd` decodes request JSON
  so malformed and trailing JSON remain rejected. The challenge permits the resulting
  compact JSON escaping. Sonic v1.15.5 supports Go 1.27 and ARM64; the benchmark
  target is amd64.
- The feed's fixed 20-post result slice is reused through a `sync.Pool` and cleared
  before reuse. It holds no response or database data between requests; it only
  avoids allocating the same container for every feed query.
- JWT verification uses a compact HMAC-SHA256 verifier with Sonic decoding into
  the three required claims. Signature and expiration validation still run for
  every request.
- Gzip runs when writing responses at least 1024 bytes, after database work. Requests
  waiting for a database connection do not reserve a compressor. Level 1 prioritizes
  compression speed; small like/error responses avoid compression CPU cost.
- `start.sh` sets a soft Go memory limit to 75% of host RAM on Linux unless already
  set, leaving room for kernel socket memory on the dedicated benchmark server.
- Request bodies are limited to 16 KiB; parser input is limited to 32 KiB.
  HS256 signatures and required expiration claims are checked on every authenticated
  request, followed by payload validation. JSON is fully parsed before writes.

## Run

On the clean Ubuntu 24.04 x86_64 benchmark box, run `install.sh` as root once.
It installs Go 1.27.3 from the official archive after checking its SHA-256.
`build.sh` runs as the normal user; `start.sh` runs in the foreground.

```bash
bash submissions/go-nbhttp-trungdlp/build.sh
DATABASE_URL=postgres://feed:password@localhost:5432/feed \
JWT_SECRET=million-users-challenge HOST=0.0.0.0 PORT=3000 EXTRA_PORT=3001 \
  bash submissions/go-nbhttp-trungdlp/start.sh
```

Only `DATABASE_URL` and `JWT_SECRET` are required. Defaults: `HOST=127.0.0.1`,
`PORT=3000`; `EXTRA_PORT` enables the second listener. GOMAXPROCS is left to Go's
CPU affinity / cgroup detection.

## Validation

```bash
cd submissions/go-nbhttp-trungdlp
go test -race ./...
go vet ./...
cd ../..
bash seed/make-seed.sh
bash test/run.sh submissions/go-nbhttp-trungdlp
```

The seed and test runner need PostgreSQL client tools and PostgreSQL 18; configure
`PGHOST`, `PGPORT`, `PGUSER`, and `PGPASSWORD` for your local test database.
The runner creates a fresh database copy, so run it only against your test server.

Verified on macOS arm64 with Go 1.27.3 and an isolated PostgreSQL 18 container:
42/42 official checks, `go test -race`, `go vet`, and shell syntax / ShellCheck.
The Linux amd64 build also passes 42/42 checks through the extra port in an
Ubuntu 24.04 container (amd64 emulation on an arm64 host).

Additional Linux checks: gzip decoding and reuse of the same TCP connection after
80 seconds idle both passed. A 40-second local mload smoke test (1000 users,
10-second ramp, gzip, both ports, think-scale 2.5) completed 2121 requests with
0 errors, p95 6.5 ms and p99 11.8 ms. These figures include Docker networking
and amd64 emulation and are not a capacity benchmark.

No million-user capacity or leaderboard score is claimed. Measure on the specified
hardware using `bench/mload`; a local test with a containerized database does not
establish the best pool size, poller count, compression level or user ceiling.

The local JSON microbenchmark on an Apple M4 Pro showed Sonic response encoding at
about 3.15 µs versus 3.42 µs for the standard library, and Sonic request decoding at
about 6.8 µs versus 17.5 µs. End-to-end gains depend on database latency and response
size; the decoder is the more useful win for this API.

The compact JWT verifier measured about 0.78 µs and 17 allocations per valid token
with its pooled HMAC state, versus 1.79 µs and 41 allocations for `golang-jwt`'s
full parser. It still checks the HS256 MAC, required numeric `exp`, and required
payload types on every call.

A fresh 25,000-user local run with the 1024-byte gzip threshold completed 180 seconds
with 0 errors, p95 1.0 ms, p99 1.6 ms, and 956 requests/s.
