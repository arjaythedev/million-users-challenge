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

## Protocol (28 Oct)

- **screen** (`scripts/screen.sh`): fresh `feed` from `feed_seed`, Postgres restarted with cold caches; the server
  as a transient systemd unit (user challenge, LimitNOFILE 8388608, CAP_NET_BIND_SERVICE) on ports 80 + 81, pinned
  with `AllowedCPUs`; Postgres, `mload` and the idle holder (`scripts/idle.c`) on the other CPUs. The idle holder
  first opens the idle connections over 127.0.0.2-9 x both ports (one GET /health each), then mload runs
  (ramp 30 s, hold 120 s, `-gzip`). The window is +60..+150 s after mload starts: busy time of the server's CPUs
  from /proc/stat (user + system + irq + softirq), the unit's cgroup CPU, `perf stat` instructions and cycles on
  the server's CPUs (from e2 on), requests from mload's 5-s ticks. One number decides: server CPU per request.
- **Standard screen = 1 server core** (CPU 0, so 1 I/O thread), 20,000 users at think-scale 0.25 (~8,450 req/s,
  about the per-core rate of #1's 1.13M users on 4 cores) + 100,000 idle connections. Postgres costs ~160-270 us
  per request on this box (the feed query), so 4 server cores at 4x the rate would need ~9 cores of Postgres.
- Calibration on 4 server cores (cal1, 8.45k req/s, 50k idle): 85.9 us/req at 19% utilisation (more wake-ups per
  request when idle).

## References and baseline (28 Oct, 1 core)

| run | us/req (CPU 0) | softirq | app cgroup (user / sys) | util | p95 | RSS at 120k conns |
|---|---|---|---|---|---|---|
| v1 r1 | 48.0 | 13.4 | 49.9 (19.7 / 30.2) | 42% | 1.8 ms | 97 MB |
| #1 rust-tokio-cknutson12 r1 | 60.5 | 14.8 | 62.1 (30.4 / 31.7) | 53% | 1.8 ms | 139 MB |
| v1 r2 | 50.9 | 13.3 | 53.1 (22.2 / 30.8) | 45% | 2.0 ms | 97 MB |
| #1 r2 | 69.7 | 14.7 | 71.7 (38.5 / 33.1) | 61% | 2.5 ms | 139 MB |

v1 costs ~20% less CPU per request than the leader at the same load, with less memory.

**Profile of v1** (perf, 20 s, 1 core): kernel 68% (TCP on both ends of loopback, io_uring, apparmor hooks,
conntrack 1.4% from docker's NAT rules on this box), libdeflate ~17.5% (`deflate_compress_fastest` 11.6%,
`deflate_flush_block` 2.4%, `deflate_make_huffman_code` 1.6%, `sort_symbols` 1.2%): ~19 us per gzipped feed.
Managed code is small. So gzip is the biggest user-space cost.

## Experiments

- **e1 wait for 128 completions or 2 ms after a busy pass** (#30; `WAIT_N=128 WAIT_US=2000`, same build, rotated
  x3): base 46.4 / 48.7 / 58.1 us/req vs 53.9 / 43.2 / 48.1 (medians 48.7 vs 48.1); p95 1.4-2.1 -> 8.0-11.0 ms. At
  ~8.5k req/s a pass rarely fills to 128, so every pass waits the full 2 ms. **Discarded** (no gain beyond the
  noise). The noise between identical runs (46-58 us) is mostly user time on this shared-vCPU host: from e2 on the
  screens also count instructions and cycles with `perf stat`.
- **e2 gzip with ISA-L igzip level 0** (`native/gz.c` shim over pinned libdeflate 1.26 and ISA-L 2.31.1; idea from
  #9 / #12). Per request, `perf stat` over 90 s, rotated x3:

  | arm | kinstr user | kinstr total | kcycles total (r1 / r2 / r3) |
  |---|---|---|---|
  | libdeflate L1 (v1) | 88.9 | 148.6-149.2 | 147.0 / 153.7 / 152.2 |
  | igzip L0 | 46.0 | 107.0-107.2 | 126.7 / 128.2 / 131.3 |
  | igzip L1 | 63.5 | 122.7-123.1 | 128.0 / 133.3 / 133.4 |

  igzip L0 halves user instructions and saves ~15% of all cycles per request. The gzipped feed grows 1,901 ->
  2,243 bytes (still 2 TCP segments). **Kept**, now the default (`GZIP_IMPL=libdeflate` brings v1 back).
  `GZIP_TRAIN=1` (retrain igzip's Huffman tables from a live body at most once a second) is in, untested.

- **mem-v1** (4 threads, holder on the same box, so slab counts both ends): RSS 66.9 MB idle, 138.6 MB at 150k,
  143.9 MB at 400k connections; slab 263 MB -> 3,638 MB, ~8.4 KB per connection pair (~4.2 KB per socket). At 3M
  that's ~12.6 GB of kernel memory for the server's sockets alone; the app itself stays flat. Health 200 after.
