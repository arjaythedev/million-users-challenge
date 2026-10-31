#!/usr/bin/env bash
# Run the repository's CI harness without taking over its shared feed_test database.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../.." && pwd)"
DB="rayo_million_check_$$"
mkdir -p "$DIR/local"
RUNNER="$(mktemp "$DIR/local/check.XXXXXX.sh")"
cleanup() {
  rm -f "$RUNNER"
  psql -qX -d postgres -c "DROP DATABASE IF EXISTS $DB" >/dev/null
}
trap cleanup EXIT
sed -e "s|^DB=feed_test$|DB=$DB|" \
    -e "s|^ROOT=.*|ROOT=\"$ROOT\"|" \
    -e 's/pkill -P $PID 2>\/dev\/null || true; //' \
    "$ROOT/test/run.sh" > "$RUNNER"
export PORT="${PORT:-18360}" EXTRA_PORT="${EXTRA_PORT:-18361}" WORKERS="${WORKERS:-3}"
bash "$RUNNER" "$DIR"
