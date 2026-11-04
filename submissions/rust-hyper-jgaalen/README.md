# Rust throughput-oriented feed API

Rust 1.98.1 on Ubuntu 24.04 x86_64, with a custom HTTP/1.1 path on Tokio and a
custom PostgreSQL wire-protocol client. This folder began as a Hyper baseline;
Hyper request/response types remain, but the submitted build serves HTTP directly.
Core dependencies: Tokio 1.54.1, httparse 1.10.1, postgres-protocol 0.6.12,
Serde JSON 1.0.154, and Hyper 1.12.0 types. All dependency versions are pinned
by Cargo.lock; builds use --locked.

## Build and run

```sh
sudo bash install.sh
bash build.sh
DATABASE_URL='postgres://feed:password@db:5432/feed' \
JWT_SECRET='million-users-challenge' HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 \
bash start.sh
```

`install.sh` installs the pinned official Rust toolchain and build prerequisites.
`build.sh` runs as a normal user. `start.sh` execs the server in the foreground.
No kernel tuning, affinity changes, database configuration changes, service
removal or reverse proxy is performed by these scripts. The runner supplies
CAP_NET_BIND_SERVICE and its file-descriptor limit. There is no restart loop.

Only HOST:PORT and HOST:EXTRA_PORT are opened (defaults 0.0.0.0:80 and :81).
There is no development-port listener in this package. Idle keep-alive connections
remain available for subsequent requests. The challenge runtime variables are
sufficient; no benchmark-specific launch environment is needed.

## Implementation

- One current-thread Tokio runtime per available CPU, with local connection tasks
  and a bounded acceptor-to-worker handoff. No CPU affinity is imposed.
- Lightweight HTTP/1.1 parsing with httparse, direct task-owned connection state,
  and lazy buffers to keep idle-connection memory small. JSON uses Serde.
- Prepared, pipelined PostgreSQL statements over a bounded custom protocol driver.
  Read SQL is unchanged: one prescribed SELECT per request, with no response,
  query-result or JWT-verification cache.
- Eight database connections per worker by default: 32 on four CPUs, 64 on eight,
  128 on sixteen. The total is validated against the 512-connection limit.
  Each worker reserves one connection for writes and chooses among read
  connections using pending-work counts. Read admission is bounded.
- Writes are grouped into transactions of up to 64 requests, waiting at most
  5 ms to collect a batch. Every successful response waits for COMMIT. Failed
  transactions are rolled back before individual replay; an uncertain commit
  is not replayed. Database durability is unchanged.
- Every authenticated request verifies its HS256 signature and expiration.
- Compression is disabled in the submitted build. This preserves the measured
  baseline and avoids adding unmeasured compression CPU cost.

The fixed build features are `direct-http-task,custom-pg,isolated-writes,least-loaded-db`.
Alternative protocol/compression implementations in the source are not enabled
by the submission build. The submitted HTTP path does not use Hyper's connection driver; the old
development Hyper patch is not shipped.

## Validation and development measurements

The final package was built from source on Ubuntu 24.04 using install.sh and
the official test/run.sh wrapper: **42/42 checks pass independently on port 80
and on port 81**, each with a fresh database clone. All 11 unit tests pass on
Linux; the submitted feature set also passes Clippy with warnings denied.

On a separate development setup (app: 4 shared vCPU / 12 GiB; PostgreSQL 18:
8 vCPU / 24 GiB; four load generators: 2 vCPU / 16 GiB each), the unmodified
`bench/mload` and its own merge command passed with **1,400,000 users**:

- 425-second browsing ramp followed by a **300-second hold**.
- Official think-scale 2.5 and Accept-Encoding: gzip; responses uncompressed.
- **p95 140.9 ms, p99 250.9 ms, zero failed requests** across ramp and hold.
- Exactly 1,400,000 dials; every full-population connection sample retained all
  1,400,000 connections. The official tool samples these counts every five seconds.
- Official ramp-inclusive throughput: **42,858 requests/s**. Full-population
  timeline windows averaged approximately **59,164 requests/s**.

**These are development results, not an official leaderboard score.** The
measurement used 20 temporary destination ports to work around source-port
allocation costs on the available load generators. That development listener
has been removed from this submission. Hardware also differs from the scoring
boxes. Earlier 10-second, 1.5M-user results used an optimized development
load-generator fork and are not presented as official qualification evidence.

Profiling identified application-host TCP processing and read-admission queuing
near saturation, while PostgreSQL retained headroom. A separate load-generator
startup bottleneck in kernel TCP tuple lookup was resolved by additional test
ports, not by changing the submitted request workload or application protocol.
