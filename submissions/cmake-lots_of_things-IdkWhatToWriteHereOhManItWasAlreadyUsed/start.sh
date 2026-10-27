#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

APPDIR=/app
mkdir -p "$APPDIR/server" "$APPDIR/launcher" "$APPDIR/postgres" "$APPDIR/listener"
cp -f server/*.cmake    "$APPDIR/server/"     || true
cp -f launcher/launcher "$APPDIR/launcher/"   || true
cp -f postgres/pool     "$APPDIR/postgres/"   || true
cp -f postgres/pool_cli "$APPDIR/postgres/"   || true
cp -f listener/server   "$APPDIR/listener/server" || true

export START_TIME="$(date +%s)"
export POOL_CLI="$APPDIR/postgres/pool_cli"

"$APPDIR/postgres/pool" 2>/tmp/pool.err &
POOL_PID=$!
trap 'kill $POOL_PID 2>/dev/null || true' EXIT
sleep 2

"$APPDIR/listener/server"