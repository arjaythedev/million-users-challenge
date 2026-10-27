# Bun, Elysia and Drizzle feed API

Submission for the Million Users Challenge. Bun 1.4.2, Elysia 1.4.30,
Drizzle ORM 0.45.4 with Bun SQL, and jose 6.2.13. PostgreSQL 18 is supplied
by the challenge. The lockfile pins transitive dependencies.

## Run

On Ubuntu 24.04, run `install.sh` as root once, then `build.sh` as the
application user. `start.sh` runs in the foreground. Configure
`DATABASE_URL`, `JWT_SECRET`, `HOST`, `PORT`, and `EXTRA_PORT` as specified
in [SPEC.md](../../SPEC.md). Both ports serve the same application.

```sh
sudo bash install.sh
bash build.sh
DATABASE_URL=postgres://feed:password@db:5432/feed \
JWT_SECRET=your-secret HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

The installer downloads the pinned official Bun release and verifies its
SHA-256 before extraction. On x86 it selects the AVX2 build when supported,
otherwise the baseline build. It also supports Linux ARM64 for local testing.
Application code is built from source. No migration runs at startup.

## Design and limits

Each Bun worker runs Elysia on both ports with Linux `SO_REUSEPORT`.
Worker count follows CPU affinity and the cgroup v2 CPU quota, capped at 32.
The foreground parent propagates signals and exits on worker failure without
restarting it. A single-worker configuration runs without a parent process.

Each worker has one Bun SQL pool shared by both listeners. The default budget
is 16 connections per worker, capped at 256 total. Integer division keeps the
aggregate below the configured limit. The application never opens more than
512 connections, including health checks. Connections remain pooled for five
minutes when idle. Statement timeout and connection establishment timeout are
five seconds. PostgreSQL durability settings are unchanged.

Drizzle constructs the prescribed selects and insert. Read queries and post
creation are built once with placeholders. Bun prepares and reuses
parameterized statements on each database connection, including the like
statement. Drizzle's preparation names are not forwarded by this adapter;
Bun owns the database statement names. There is no result cache.

Each request executes one SQL statement. SQL values are parameterized. The
database unique constraint resolves concurrent duplicate likes, and the
affected-row count distinguishes first and repeated likes. Only the post
foreign-key failure becomes `post not found`. A missing user remains an
internal error. New post author names come from the verified token, as the
specification requires.

Database admission occurs after authentication and body validation. At most
as many queries as configured pool connections are admitted at once.
Running and queued SQL work together cannot exceed the shared HTTP budget of
2,048 requests per worker. Queued work waits at most 500 ms. Pool size controls
SQL concurrency independently of this burst budget. Both the timer and grant path
check expiration. Expired or aborted waiters never issue SQL. Running writes
are awaited through commit, and are never retried automatically. An additional
limit of 2,048 active HTTP handlers per worker bounds work before SQL admission.
Idle keep-alive connections consume no handler or database admission slots.

HTTP idle timeout is 120 seconds. JSON bodies are limited to 16 KiB and five
seconds of reading. Authentication precedes body parsing and ID validation.
HS256 signatures, expiration, and payload fields are checked on every protected
request. Only key bytes are reused. Unicode body length follows PostgreSQL's
code-point semantics. Responses use ordered objects and standard JSON escaping.
Dates are converted to UTC milliseconds without rounding microseconds upward.

Responses of at least 1,024 UTF-16 code units use gzip level 1 when accepted.
Compression is per response; idle connections retain no compression context.
`q=0`, wildcard negotiation, and `Vary: Accept-Encoding` are handled.
Unexpected errors produce rate-limited structured diagnostics without query
parameters, post bodies, tokens, or connection strings. Shutdown allows twenty
seconds for HTTP draining and database cleanup before failing.

This is the challenge's direct HTTP deployment. A public deployment also needs
its own TLS, secret management, backups, recovery and availability arrangements.
The fixed challenge schema does not provide persisted post-creation idempotency
keys or token revocation.

## Verify

Run the upstream harness from the repository root against a fresh copy of the
official seed. It builds the submission and checks all 42 assertions.

```sh
bash test/run.sh submissions/bun-elysia-youssefsz
```

Run the additional tests inside this folder. Database integration tests require
an isolated writable copy of `feed_seed`, never the template itself.

```sh
bun run check
bun run format:check
TEST_DATABASE_URL=postgres://postgres:password@localhost:5432/feed_dev bun test
```

The additional tests cover concurrent duplicate likes, expiration without token
verification caching, malformed input precedence, Unicode, microseconds, gzip,
database failure, queue expiry without later writes, delayed timer callbacks,
slow body cancellation, and slow uploads not occupying database slots.
A 350 ms PostgreSQL lock regression checks that a bounded burst of queued
writes completes and persists after the lock releases.

## Local benchmark results

See [BENCHMARKS.md](BENCHMARKS.md) for measured results, raw output, and the
limits of these local tests. The official server score is unmeasured.

## Local benchmark tools

`tools/local-bench.py` resets only the disposable `feed_bench` database, starts
the application, checks both listeners, warms it for seven seconds, runs the
upstream `bench/mload/main.go` workload, and records raw results and cgroup
CPU and memory statistics in `results/`. `tools/lifecycle.py` checks an actual
80-second idle TCP connection, shutdown during a blocked write, and worker
crash propagation. The lifecycle script uses a writable `feed_dev` database.

The tools expect a Docker network named `million-bench`, a PostgreSQL container
named `million-postgres`, and a runtime image named `million-bun:1.4.2`.
Create these from the repository root. These commands use a local test password.

```sh
docker network create million-bench
docker run -d --name million-postgres --network million-bench \
  --cpus=4 --memory=3g -e POSTGRES_PASSWORD=challenge-local \
  -p 127.0.0.1:55432:5432 postgres:18-alpine \
  -c max_connections=600 -c shared_buffers=512MB \
  -c effective_cache_size=2GB -c maintenance_work_mem=256MB \
  -c jit=off -c enable_memoize=off
docker build -t million-bun:1.4.2 \
  -f submissions/bun-elysia-youssefsz/tools/Dockerfile \
  submissions/bun-elysia-youssefsz
PGHOST=127.0.0.1 PGPORT=55432 PGUSER=postgres \
PGPASSWORD=challenge-local bash seed/make-seed.sh
mkdir -p submissions/bun-elysia-youssefsz/tools/bin
go build -o submissions/bun-elysia-youssefsz/tools/bin/mload bench/mload/main.go
GOOS=linux GOARCH=arm64 CGO_ENABLED=0 go build \
  -o submissions/bun-elysia-youssefsz/tools/bin/mload-linux bench/mload/main.go
```

Use `GOARCH=amd64` instead on x86 Docker hosts. The Go source was compiled
directly with Go 1.25.6 for local measurements; the upstream source is unchanged.
Build the application, then run a short accelerated comparison from this folder.

```sh
bash build.sh
python3 tools/local-bench.py trial --generator linux --workers 4 --pool 64 \
  --users 4000 --think-scale 0.05 --ramp 10s --hold 30s
```

For a five-minute hold with the challenge's think time, use `--think-scale 2.5
--ramp 60s --hold 5m`. Large user counts need enough generator memory and source
ports. The Linux generator runs in its own container, avoiding Docker Desktop's
host port-forwarding path. Generated binaries and result files are ignored by Git.

Use `--generator linux --generators 4` to split a large run across four
containers with different source IPs. The tool synchronizes their start times,
assigns distinct user offsets, and merges their histograms using upstream mload.
Each generator then opens fewer connections per target port. A single generator
can become the limit during ramp even when the application has CPU headroom.

Optional application CLI flags `--workers N`, `--pool N`, and `--gzip-level N`
support local comparisons. Normal `start.sh` needs no extra settings. On macOS,
use `bun dist/main.js --workers 1`; `SO_REUSEPORT` process sharing requires Linux.

Local Docker measurements share physical CPU, RAM and storage with the database
and generator. They cannot establish a score on the owner's separate x86 VPSs.
