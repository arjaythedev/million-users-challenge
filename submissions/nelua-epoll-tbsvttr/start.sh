#!/usr/bin/env bash
# Foreground (systemd ExecStart). Config from env: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT (optional),
# DB_POOL (optional, Postgres connections in total), WORKERS (optional, default: one per CPU this process may use,
# less the NIC's interrupt CPU), BATCH_US (optional, how long a worker waits after a quiet loop pass, default 2000).
# For A/B runs: SKIP_IRQ_CPU, PIN, STEER, QUICKACK, QUICKACK_MS, DEFER_ACCEPT (see README.md).
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$DIR/build/feed"
