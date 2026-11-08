#!/usr/bin/env bash
# Runs the server in the foreground (systemd ExecStart). Config: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT.
# Optional tuning (defaults are what gets benchmarked): THREADS, DB_READ_CONNS, DB_WRITE_CONNS, GZIP_LEVEL.
set -euo pipefail
# Every client connection is a file descriptor. Under systemd LimitNOFILE sets soft = hard already; from a login
# shell the soft limit is often 1024, and tokio does not raise it itself.
ulimit -n "$(ulimit -Hn)"
exec "$(cd "$(dirname "$0")" && pwd)/target/release/feed-server"
