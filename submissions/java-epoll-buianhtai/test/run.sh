#!/usr/bin/env bash
# Linux; requires Python 3 plus the repository's normal test prerequisites. Uses the same throwaway feed_test DB.
set -euo pipefail
DIR="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$DIR/../.." && pwd)"
bash "$ROOT/test/run.sh" "$DIR"
export HOST=127.0.0.1 PORT="${PORT:-3000}" EXTRA_PORT="${EXTRA_PORT:-3001}" JWT_SECRET=million-users-challenge
export DATABASE_URL="postgres://${PGUSER:-$(whoami)}${PGPASSWORD:+:$PGPASSWORD}@${PGHOST:-localhost}:${PGPORT:-5432}/feed_test"
log="$(mktemp)"
bash "$DIR/start.sh" >"$log" 2>&1 &
pid=$!
cleanup() { kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; rm -f "$log"; }
trap cleanup EXIT
ready=0
for _ in {1..60}; do
  if curl -sf "http://$HOST:$PORT/health" >/dev/null; then ready=1; break; fi
  kill -0 "$pid" 2>/dev/null || break
  sleep 1
done
if [ "$ready" != 1 ]; then cat "$log"; exit 1; fi
python3 "$DIR/test/protocol.py"
