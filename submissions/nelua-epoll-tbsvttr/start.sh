#!/usr/bin/env bash
# Foreground (systemd ExecStart). Config from env: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT (optional),
# DB_POOL (optional, Postgres connections in total), WORKERS (optional, default: one per CPU this process may use),
# BATCH_US (optional, how long a worker waits after a quiet loop pass, default 2000).
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
export WORKERS="${WORKERS:-$(nproc)}"
exec "$DIR/build/feed"
