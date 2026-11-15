#!/usr/bin/env bash
# Runs the server in the foreground. Config: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT
# (optional tuning: WORKERS, DB_CONNS, READ_GROUP_MAX, GZIP_MIN; see README.md).
set -euo pipefail
exec "$(cd "$(dirname "$0")" && pwd)/bin/feed"
