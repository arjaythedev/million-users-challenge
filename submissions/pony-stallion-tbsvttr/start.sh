#!/usr/bin/env bash
# Foreground (systemd ExecStart). Config from env: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT (optional),
# DB_POOL (optional, Postgres connections in total). The Pony runtime starts one scheduler thread per core.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$DIR/build/feed"
