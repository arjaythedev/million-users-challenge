#!/usr/bin/env bash
# Foreground (systemd ExecStart). Config from env: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT (optional),
# DB_POOL (optional, Postgres connections per worker), WORKERS (optional, default: one per CPU we may run on).
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
BUN=/opt/bun/bin/bun; [ -x "$BUN" ] || BUN=bun
# nproc reads the affinity mask, so systemd's AllowedCPUs/CPUAffinity or taskset set the worker count.
export WORKERS="${WORKERS:-$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN)}"
export NODE_ENV=production
exec "$BUN" "$DIR/dist/server.js"
