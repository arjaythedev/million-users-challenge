#!/usr/bin/env bash
# Foreground (systemd ExecStart). Config from env: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT (optional),
# DB_POOL (optional, Postgres connections in total). The BEAM starts one scheduler per CPU it may run on.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
export RELEASE_DISTRIBUTION=none   # a single node: no epmd, no node name
exec "$DIR/_build/prod/rel/feed/bin/feed" start
