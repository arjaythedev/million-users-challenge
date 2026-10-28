# Worklog (F# million users, delneg)

All numbers are local, on a Synteq box (8 shared vCPUs, Xeon Platinum 8260, 16 GB, Ubuntu 24.04, kernel
6.8.0-147), not the grader's box. Postgres 18 on the same box with the challenge's `box/postgresql.conf` scaled to
16 GB (shared_buffers 2 GB, max_wal_size 2 GB for the small disk; jit off, memoize off, max_connections 600), the
grader's sysctls from `box/setup-app-box.sh`.

## Phase 0 (28 Oct)

- Upstream changed the scoring on 28 Oct: three scores (16, 8, 4 cores), 16 capped at 1,000,000, 8 and 4 searched
  to 3,000,000; LimitNOFILE 8,388,608. #1 rust-tokio-cknutson12 is merged and scored 1,000,000 (max) / 1,210,000 /
  1,130,000; its failing probes (1.24M on 8, 1.16M on 4) failed on latency with app CPU 48% / 79%.
  On 4 cores at 1.13M: 79% of 4 cores for 35.9k req/s = ~88 us of CPU per request, kernel included. The bar.
- Box: feed_seed built by seed/make-seed.sh as postgres, then the tables handed to role `feed` (the tests run as
  `feed`). Reference #1 built from upstream main: 42/42.
- v1: 42/42 on the buffer ring (registered on the `resv[0] = 1` retry: -147 has the inverted check) and with
  `LEGACY_BUFS=1`. Manual: gzip body identical to the plain one (1,901 B), HTTP/1.0 keep-alive, `/health` 503 while
  Postgres is stopped and 200 again ~3 s after it starts.
