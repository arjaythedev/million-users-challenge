#!/usr/bin/env bash
# Runs the server in the foreground. Config from the environment: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT.
# Optional knobs for experiments (defaults in src/main.rs): THREADS, PG_READ_CONNS, PG_WRITE_CONNS (per thread),
# GZIP_LEVEL, GZIP_MIN.
set -euo pipefail
# Under systemd soft = hard = LimitNOFILE already; from a shell the soft limit is often 1024.
ulimit -n "$(ulimit -Hn)"
cd "$(dirname "$0")"
if [[ "${PROFILING:-0}" = 1 ]]; then exec ./target/profiling/feed; fi
exec ./target/release/feed
