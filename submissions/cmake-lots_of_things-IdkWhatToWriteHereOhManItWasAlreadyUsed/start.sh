#!/usr/bin/env bash

set -euo pipefail

ulimit -n "$(ulimit -Hn)" 2>/dev/null || true

APPDIR="${APPDIR:-/app}"
export START_TIME="$(date +%s)"
export POOL_CLI="$APPDIR/postgres/pool_cli"

"$APPDIR/postgres/pool" 2>/tmp/pool.err &
POOL_PID=$!
trap 'kill $POOL_PID 2>/dev/null || true' EXIT
sleep 2

exec "$APPDIR/listener/server"