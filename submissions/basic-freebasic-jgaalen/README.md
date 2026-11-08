# GOTO MILLION — FreeBASIC feed API

A social feed API in FreeBASIC 1.10.1. HTTP parsing, routing, JWT validation
policy, JSON output, asynchronous database scheduling, and worker lifecycle
are written in BASIC. The small C adapter handles Linux socket setup and
Jansson's inline reference-count helper.

## Build and run

```sh
sudo bash install.sh
bash build.sh
DATABASE_URL='postgres://feed:password@db/feed' \
JWT_SECRET='million-users-challenge' HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 \
bash start.sh
```

The installer builds FreeBASIC 1.10.1 from its SHA-256-verified source bootstrap
archive. Native dependencies are Ubuntu 24.04 packages: libevent 2.1.12,
Jansson 2.14, libpq 16 (compatible with PostgreSQL 18), and OpenSSL 3.0.
Tested package revisions were respectively `2.1.12-stable-9ubuntu2.2`,
`2.14-2build2`, `16.15-0ubuntu0.24.04.1`, and `3.0.13-0ubuntu3.16`.
The scripts install no reverse proxy and make no kernel or database changes.

## Implementation

- One process per online CPU with SO_REUSEPORT listeners and libevent's Linux
  epoll backend. No CPU affinity is imposed.
- Nonblocking HTTP/1.1 sockets, persistent connections, lazy buffers, reused
  event objects, and immediate nonblocking response writes.
- Prepared asynchronous libpq queries, with the prescribed SQL unchanged.
  A bounded request queue feeds each worker's pool.
- Each statement's result is retained until libpq reaches ReadyForQuery after
  Sync. Only then is the HTTP response sent, so writes are committed first.
  A deferred commit error supersedes an earlier successful statement result.
- No response, query-result, or JWT-verification caches. Every authenticated
  request checks HS256, its signature, expiration, and required claim types.
- JSON output escaping and Unicode body trimming/counting are implemented in
  BASIC; Jansson validates input JSON, and OpenSSL provides HMAC-SHA256.
- Compression is disabled. Both required listeners are served by the same
  implementation, and idle keep-alive connections remain available.

The standard runtime environment is sufficient. Defaults are 32 database
connections per worker, with a hard total limit of 512. `BASIC_WORKERS` and
`BASIC_POOL` allow development overrides; `DEV_EXTRA_PORTS` optionally enables
additional development listeners. Without that override, only PORT and
EXTRA_PORT are opened (defaults 80 and 81).

## Validation

The submitted application passes all 42 official correctness checks on each
of two listeners, independently against fresh database clones. Additional
checks cover JSON control-character and Unicode round-trips, 500/501 Unicode
character limits, Unicode whitespace trimming, malformed JSON, JWT claim types,
split requests, 20 pipelined requests, and an 80-second keep-alive idle period.

A PostgreSQL protocol regression test withholds Sync after flushing an INSERT's
result. The application sends no HTTP response while the transaction remains
uncommitted and continues serving unrelated requests. After Sync is released,
the write returns 201 and reads back successfully.

## Development measurement — not an official score

On a separate development setup (app: 4 shared vCPU / 12 GiB; PostgreSQL 18:
8 vCPU / 24 GiB; four generators: 2 vCPU / 16 GiB each), the unchanged official
`bench/mload` and its merger passed with **1,000,000 users**:

- 304-second ramp followed by a **300-second hold**.
- Four workers and 16 database connections per worker (64 total).
- Official think-scale 2.5 and Accept-Encoding: gzip; responses uncompressed.
- **p95 96.54 ms, p99 216.14 ms, zero failed requests**, counting ramp and hold.
- 19,640,898 requests and exactly 1,000,000 TCP dials.
- Every full-population sample retained all 250,000 connections on each of the
  four generators: 57 samples each, at five-second intervals.
- Ramp-inclusive throughput: **32,518 requests/s**. Full-population timeline
  windows averaged approximately **42,250 requests/s**.
- Hold process CPU averaged approximately 2.98 cores; peak aggregate process
  RSS was 456 MiB, excluding kernel TCP memory and potentially double-counting
  shared mappings.

**This is development evidence, not an official leaderboard score.** The run
used 20 development destination ports to avoid source-port allocation overhead
on the available generators. Hardware differs from the scoring boxes, and the
measurement does not establish one-million capacity on the official two-port
topology. The test used the same application code with development overrides;
no load-generator detection or benchmark-specific response logic is present.
