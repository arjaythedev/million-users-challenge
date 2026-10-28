#!/usr/bin/env bash
# Foreground (systemd ExecStart). Config from env: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT (optional);
# optional WORKERS (total PHP workers, default one per CPU, split over the two servers) and DB_POOL (Postgres
# connections per worker, default 8: a worker serves many requests at once, each borrowing a connection per statement).
set -euo pipefail
cd "$(dirname "$0")"
export APP_ENV=production OCTANE_SERVER=swoole
export APP_KEY="${APP_KEY:-base64:$(head -c 32 /dev/zero | base64)}"   # nothing is encrypted; the framework just wants one

cpus=$(nproc)   # respects the CPU affinity of this process
servers=1; [ -n "${EXTRA_PORT:-}" ] && servers=2
workers=${WORKERS:-$cpus}
[ "$workers" -gt 480 ] && workers=480
per=$(( workers / servers )); [ "$per" -lt 1 ] && per=1
# rule 5: at most 512 Postgres connections in total = workers x pool (480 at most)
pool=${DB_POOL:-8}; [ $(( workers * pool )) -gt 480 ] && pool=$(( 480 / workers )); [ "$pool" -lt 1 ] && pool=1
export DB_POOL=$pool
reactors=$(( cpus / servers )); [ "$reactors" -lt 1 ] && reactors=1
export SWOOLE_REACTORS=$reactors
mkdir -p storage/logs storage/framework/{cache,sessions,views}

# Octane listens on one port, so one Octane server per port; the load is split evenly over the ports.
start() { # <port> <state file>
  OCTANE_STATE_FILE="$PWD/storage/logs/$2.json" exec php artisan octane:start --server=swoole --host="${HOST:-0.0.0.0}" \
    --port="$1" --workers="$per" --task-workers=0 --max-requests="${MAX_REQUESTS:-100000}"
}
pids=()
(start "${PORT:-80}" octane-main) & pids+=($!)
[ "$servers" = 2 ] && { (start "$EXTRA_PORT" octane-extra) & pids+=($!); }
trap 'kill "${pids[@]}" 2>/dev/null || true; wait' TERM INT
wait -n "${pids[@]}" || true
kill "${pids[@]}" 2>/dev/null || true
wait || true
exit 1   # one server died: not restarted (rule 10), so don't pretend to be healthy
