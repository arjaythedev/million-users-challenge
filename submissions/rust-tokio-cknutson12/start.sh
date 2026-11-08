#!/usr/bin/env bash
# Runs the server in the foreground (systemd ExecStart). Config: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT;
# optional THREADS, DB_READ_CONNS, DB_WRITE_CONNS, GZIP_LEVEL, GZIP_MIN (see README.md).
set -euo pipefail
# Soft open-files limit = hard limit: a no-op under systemd (LimitNOFILE sets both), needed from a login shell.
ulimit -n "$(ulimit -Hn)"
exec "$(cd "$(dirname "$0")" && pwd)/target/release/feed-server"
