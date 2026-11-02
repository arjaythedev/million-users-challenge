#!/usr/bin/env bash
# Runs the server in the foreground (systemd ExecStart). Config: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT;
# optional WORKERS, DB_CONNS, GZIP_LEVEL, GZIP_MIN (see README.md).
set -euo pipefail
cd "$(dirname "$0")"
# Soft open-files limit = hard limit: a no-op under systemd (LimitNOFILE sets both), needed from a login shell.
ulimit -n "$(ulimit -Hn)"
exec .venv/bin/python -m feed
