#!/usr/bin/env bash
# Profiling run (macOS, Postgres in docker): build, reset the DB, start the server, drive it with mload, capture pprof over the hold.
#
# Usage: bench/bench-run.sh            -> writes to profiles/run<N+1> (gitignored)
#        OUT=/some/dir USERS=60000 bench/bench-run.sh
# Env:   USERS (40000), RAMP (60s), HOLD (5m), LAN_IP (en0 address), PG_CONTAINER (mucpg)
#        Server tuning (DB_POOL, READ_WORKERS, WRITE_WORKERS, ...) passes straight through to start.sh.
set -euo pipefail

SUB="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$SUB/../.." && pwd)"
PROFILES="$SUB/profiles"

USERS=${USERS:-40000}
RAMP=${RAMP:-60s}
HOLD=${HOLD:-5m}
PG_CONTAINER=${PG_CONTAINER:-mucpg}
LAN_IP=${LAN_IP:-$(ipconfig getifaddr en0 2>/dev/null || true)}
DBG=http://127.0.0.1:6060/debug/pprof

to_secs() { case "$1" in *m) echo $(( ${1%m} * 60 ));; *s) echo "${1%s}";; *) echo "$1";; esac; }
RAMP_S=$(to_secs "$RAMP")
HOLD_S=$(to_secs "$HOLD")

# Next free profiles/runN unless OUT is given (the original run lives in profiles/ itself).
if [ -z "${OUT:-}" ]; then
  n=2
  while [ -e "$PROFILES/run$n" ]; do n=$((n + 1)); done
  OUT="$PROFILES/run$n"
fi
mkdir -p "$OUT"

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
die() { log "ERROR: $*" >&2; exit 1; }

# --- tools -------------------------------------------------------------------------------------
MLOAD="$ROOT/bench/mload/mload"
if [ ! -x "$MLOAD" ] || [ -n "$(find "$ROOT/bench/mload" -name '*.go' -newer "$MLOAD")" ]; then
  log "building mload"
  (cd "$ROOT/bench/mload" && go build -o mload .)
fi
command -v docker >/dev/null || die "docker not found"
docker ps --format '{{.Names}}' | grep -qx "$PG_CONTAINER" || die "postgres container '$PG_CONTAINER' is not running"
[ -f "$ROOT/seed/tokens.json" ] || die "missing seed/tokens.json (run: bash seed/make-seed.sh)"
psql() { docker exec -i -e PGUSER=postgres "$PG_CONTAINER" psql "$@"; }

TARGETS=127.0.0.1
[ -n "$LAN_IP" ] && TARGETS="127.0.0.1,$LAN_IP"

log "output:  $OUT"
log "load:    $USERS users, ramp $RAMP, hold $HOLD, targets $TARGETS:3000,3001"

# --- build + reset -----------------------------------------------------------------------------
log "building server"
bash "$SUB/build.sh"

log "resetting database from feed_seed"
psql -qX -d postgres -c "DROP DATABASE IF EXISTS feed WITH (FORCE)" -c "CREATE DATABASE feed TEMPLATE feed_seed"

# --- server ------------------------------------------------------------------------------------
export DATABASE_URL=postgres://postgres@localhost:5432/feed JWT_SECRET=million-users-challenge
export HOST=0.0.0.0 PORT=3000 EXTRA_PORT=3001 DEBUG_PORT=6060 GOMEMLIMIT=${GOMEMLIMIT:-8GiB}
env | grep -E '^(DB_POOL|READ_WORKERS|WRITE_WORKERS|GROUP_COMMIT|GOMEMLIMIT|GOGC|GOMAXPROCS)=' \
  | sort > "$OUT/env.txt" || true

bash "$SUB/start.sh" > "$OUT/server.log" 2>&1 &
SPID=$!
SAMPLER=""
MPID=""
cleanup() {
  for p in $MPID $SAMPLER $SPID; do kill "$p" 2>/dev/null || true; done
  pkill -f "$MLOAD -target" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT
trap 'log "interrupted"; exit 130' INT TERM

log "waiting for server (pid $SPID)"
up=""
for _ in $(seq 60); do
  kill -0 "$SPID" 2>/dev/null || { tail -20 "$OUT/server.log" >&2; die "server exited during startup"; }
  if curl -sf http://127.0.0.1:3000/health >/dev/null && curl -sf http://127.0.0.1:3001/health >/dev/null \
     && curl -sf -o /dev/null "$DBG/"; then up=1; break; fi
  sleep 1
done
[ -n "$up" ] || { tail -20 "$OUT/server.log" >&2; die "server not healthy after 60s (ports 3000/3001 + debug 6060)"; }
FPID=$(pgrep -f "$SUB/bin/feed" | head -1)
log "server up (feed pid $FPID)"

# --- resource sampler: server CPU/RSS/threads, postgres container CPU ----------------------------
( echo "t_s,server_cpu_pct,server_rss_mb,server_threads,pg_cpu_pct"
  t0=$(date +%s)
  while kill -0 "$FPID" 2>/dev/null; do
    read -r cpu rss < <(ps -o %cpu=,rss= -p "$FPID")
    thr=$(ps -M -p "$FPID" | tail -n +2 | wc -l | tr -d ' ')
    pg=$(docker stats --no-stream --format '{{.CPUPerc}}' "$PG_CONTAINER" | tr -d '%')
    echo "$(( $(date +%s) - t0 )),$cpu,$((rss / 1024)),$thr,$pg"
    sleep 10
  done ) > "$OUT/resources.csv" &
SAMPLER=$!

# --- load: progress streams to the terminal and mload.log ----------------------------------------
"$MLOAD" -target "$TARGETS" -ports 3000,3001 -users "$USERS" -ramp "$RAMP" -hold "$HOLD" \
  -gzip -think-scale 2.5 -tokens "$ROOT/seed/tokens.json" -out "$OUT/run.json" 2>&1 \
  | tee "$OUT/mload.log" | sed -u 's/^/  mload | /' &
MPID=$!

sleep $((RAMP_S + 2))
log "ramp done, capturing hold-start heap/allocs/goroutines"
curl -s -o "$OUT/heap-hold-start.pb.gz"   "$DBG/heap"
curl -s -o "$OUT/allocs-hold-start.pb.gz" "$DBG/allocs"
curl -s "$DBG/goroutine?debug=1" | head -1 > "$OUT/goroutines-hold-start.txt"

CPU_S=$((HOLD_S - 5)); [ "$CPU_S" -lt 10 ] && CPU_S=10
log "cpu profile: ${CPU_S}s"
curl -s -o "$OUT/cpu-hold-${CPU_S}s.pb.gz" "$DBG/profile?seconds=$CPU_S"

log "capturing hold-end heap/allocs/goroutines/vars"
curl -s -o "$OUT/heap-hold-end.pb.gz"      "$DBG/heap"
curl -s -o "$OUT/allocs-hold-end.pb.gz"    "$DBG/allocs"
curl -s "$DBG/goroutine?debug=1" | head -1 > "$OUT/goroutines-hold-end.txt"
curl -s -o "$OUT/goroutine-hold-end.pb.gz" "$DBG/goroutine"
curl -s -o "$OUT/vars-hold-end.json"       http://127.0.0.1:6060/debug/vars

log "waiting for mload to finish"
wait "$MPID" || true
MPID=""

echo
log "done. results in $OUT"
ls -1 "$OUT" | sed 's/^/  /'
echo
echo "  go tool pprof -http=:8080 $OUT/cpu-hold-${CPU_S}s.pb.gz"
