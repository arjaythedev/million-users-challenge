# Worklog (fsharp-aspnetcore-delneg)

All numbers are **local, NOT official**: a Synteq VPS (8 shared vCPUs, Xeon Platinum 8260, 16 GB, Ubuntu 24.04,
kernel 6.8.0-147) with Postgres 18 on the same box (challenge `postgresql.conf` scaled to 16 GB) and the grader's
sysctls. Server on CPUs 0-3, Postgres + mload + idle holder on 4-7 (`scripts/screen.sh`).

## v1 (30 Oct): first correct version

- `src/Program.fs` 215 lines. `test/run.sh` 42/42 on port 3000 and with the ports swapped (`scripts/conformance.sh`).
- Manual: gzipped `/feed` (2,303 B) unzips to the same bytes as the plain one; `/health` 503
  `{"status":"degraded","db":"unreachable",...}` with Postgres stopped, 200 again ~2 s after it starts.
