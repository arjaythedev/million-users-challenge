#!/usr/bin/env bash
set -euo pipefail

export PATH="/opt/cmake/bin:$PATH"
export DATABASE_URL JWT_SECRET HOST PORT EXTRA_PORT

postgres/pool &
POOL_PID=$!
sleep 1

exec listener/server