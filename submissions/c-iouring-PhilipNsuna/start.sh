#!/usr/bin/env bash
# Runs the server in the foreground. Config: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT (SPEC.md).
# Optional knobs (README.md): LOOPS, DB_READ, DB_WRITE, BATCH_MIN, BATCH_MICROS, GZIP_MIN, PIN, CBPF, URING, STATS_SECS.
set -euo pipefail
cd "$(dirname "$0")"              # feed_sample.json is read from here
ulimit -n "$(ulimit -Hn)"         # every connection is a file descriptor (a no-op under systemd's LimitNOFILE)
exec ./bin/feed
