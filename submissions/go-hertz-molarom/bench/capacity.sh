#!/usr/bin/env bash
# Local capacity estimate: measure what scales with users (memory per connection, CPU per request)
# on this machine, then extrapolate to the 4/8/16-core, 32 GB scoring box.
#   macOS: run macos-limits.sh apply first; Postgres in the docker container $PG_CONTAINER.
#   Linux (the Lima VM, see linux-vm.sh): run as root; server, Postgres and mload pinned to their own cores.
#
# Usage: bench/capacity.sh [stairs|hot|ladder|mixed|all]    (default all; each phase gets a fresh server + DB)
#
#   stairs  users added in steps at the scoring pace (think-scale 2.5); per step: RSS, live heap, stacks,
#           core-ms/request. Slopes give bytes per connection and how CPU per request moves with conns.
#   hot     few users, tiny think time, ~HOT_RPS req/s: CPU per request at the 1M-user request rate.
#   ladder  the hot phase once per LADDER_RPS level: where the (pinned) server stops passing.
#   mixed   BASE_USERS at the scoring pace, then the hot load on top: CPU per request with a big heap.
#
# Env (defaults): STEPS (25000,...,150000 cumulative), STEP_RATE (2500 new users/s), SETTLE (60 s after each
#   ramp; the last MEASURE=30 s are measured), HOT_USERS (10000), HOT_RPS (42000), HOT_RAMP (20), HOT_HOLD (90),
#   LADDER_RPS (20000,40000,60000,80000), BASE_USERS (150000), OUT (profiles/cap<N>, gitignored), GOMEMLIMIT (12GiB).
#   macOS: PG_CONTAINER (mucpg). Linux: SERVER_CPUS (0-3), MLOAD_CPUS (8-11), TARGET_IPS (16), PG_UNIT
#   (postgresql@18-main). Server tuning env (DB_POOL, READ_WORKERS, ...) passes through to start.sh.
set -euo pipefail

SUB="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$SUB/../.." && pwd)"

# In the Lima VM this runs from the /opt/muc copy: re-sync it from the repo mount (and rebuild) first,
# then re-exec the fresh copy of this script, so a run never benchmarks stale sources.
if [ -f "$ROOT/.muc-src" ] && [ -z "${MUC_SYNCED:-}" ]; then
  MUC_SRC=$(cat "$ROOT/.muc-src")
  bash "$MUC_SRC/submissions/go-hertz-molarom/bench/linux-vm.sh" sync
  MUC_SYNCED=1 exec bash "$SUB/bench/capacity.sh" "$@"
fi
PROFILES="$SUB/profiles"   # results only (gitignored); the scripts live in bench/
MODE=${1:-all}
OS=$(uname -s)

STEPS=${STEPS:-25000,50000,75000,100000,125000,150000}
STEP_RATE=${STEP_RATE:-2500}
SETTLE=${SETTLE:-60}
MEASURE=${MEASURE:-30}
HOT_USERS=${HOT_USERS:-10000}
HOT_RPS=${HOT_RPS:-42000}
HOT_RAMP=${HOT_RAMP:-20}
HOT_HOLD=${HOT_HOLD:-90}
LADDER_RPS=${LADDER_RPS:-20000,40000,60000,80000}
BASE_USERS=${BASE_USERS:-150000}
PG_CONTAINER=${PG_CONTAINER:-mucpg}
SERVER_CPUS=${SERVER_CPUS:-0-3}
MLOAD_CPUS=${MLOAD_CPUS:-8-11}
TARGET_IPS=${TARGET_IPS:-16}
PG_UNIT=${PG_UNIT:-postgresql@18-main}
SCORE_THINK=2.5
PORTS=3000,3001
DBG=http://127.0.0.1:6060

if [ -z "${OUT:-}" ]; then
  n=1
  while [ -e "$PROFILES/cap$n" ]; do n=$((n + 1)); done
  OUT="$PROFILES/cap$n"
fi
mkdir -p "$OUT"

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "$OUT/capacity.log"; }
die() { log "ERROR: $*" >&2; exit 1; }
now() { date +%s; }

# --- preflight -----------------------------------------------------------------------------------
ulimit -n "$(ulimit -Hn)" 2>/dev/null || ulimit -n 1048576 2>/dev/null || true
if [ "$OS" = Darwin ]; then
  [ "$(sysctl -n net.inet.ip.portrange.first)" -le 1024 ] || die "port range not raised: run bench/macos-limits.sh apply"
  [ "$(sysctl -n kern.ipc.somaxconn)" -le 32767 ] || die "kern.ipc.somaxconn > 32767 makes Go listeners reset connections: sudo sysctl -w kern.ipc.somaxconn=32767"
  TARGETS=$(ifconfig lo0 | awk '/inet 127\./{print $2}' | paste -sd, -)
  [ "$(echo "$TARGETS" | tr ',' '\n' | wc -l)" -ge 4 ] || die "need loopback aliases: run bench/macos-limits.sh apply"
  docker ps --format '{{.Names}}' | grep -qx "$PG_CONTAINER" || die "postgres container '$PG_CONTAINER' is not running"
  psql() { docker exec -i -e PGUSER=postgres "$PG_CONTAINER" psql "$@"; }
  pin_server() { "$@"; }
  pin_mload() { "$@"; }
  NCPU=$(sysctl -n hw.ncpu) MEM_GIB=$(( $(sysctl -n hw.memsize) / 1073741824 ))
  DATABASE_URL=${DATABASE_URL:-postgres://postgres@localhost:5432/feed}
else
  [ "$(id -u)" = 0 ] || die "run as root on Linux (fd limits, drop_caches, cgroup reads)"
  [ "$(ulimit -n)" -ge 1048576 ] || die "ulimit -n is $(ulimit -n); raise fs.nr_open / the hard limit"
  TARGETS=$(seq -s, -f '127.0.0.%g' 1 "$TARGET_IPS")   # all of 127/8 is local on Linux: no aliases needed
  psql() { sudo -u postgres psql "$@"; }
  # Like the box's systemd unit: the server in its own scope on SERVER_CPUS, and everything else (this
  # script, samplers, mload, lima's agent, other services) moved off those cpus, so their busy time is
  # the server's plus the kernel work done there.
  OTHER_CPUS=$(python3 -c "
def cpus(s):
    out=set()
    for part in filter(None, s.split(',')):
        a,_,b=part.partition('-'); out |= set(range(int(a), int(b or a)+1))
    return out
print(','.join(map(str, sorted(set(range($(nproc --all))) - cpus('$SERVER_CPUS')))))")
  for unit in user.slice system.slice init.scope; do
    systemctl set-property --runtime "$unit" AllowedCPUs="$OTHER_CPUS" >/dev/null 2>&1 || true
  done
  pin_server() { systemd-run --scope --quiet --slice=challenge.slice -p AllowedCPUs="$SERVER_CPUS" "$@"; }
  pin_mload() { taskset -c "$MLOAD_CPUS" "$@"; }
  NCPU=$(nproc --all) MEM_GIB=$(( $(awk '/^MemTotal:/{print $2}' /proc/meminfo) / 1048576 ))
  DATABASE_URL=${DATABASE_URL:-postgres://feed:feed@127.0.0.1:5432/feed}
  PG_CG=/sys/fs/cgroup$(systemctl show -P ControlGroup "$PG_UNIT")
  [ -r "$PG_CG/cpu.stat" ] || die "can't read $PG_CG/cpu.stat (is $PG_UNIT running?)"
  # SERVER_CPUS as a space-separated list of cpu numbers, for /proc/stat
  SRV_CPU_LIST=$(python3 -c "
s='$SERVER_CPUS'; out=[]
for part in s.split(','):
    a,_,b=part.partition('-'); out += list(range(int(a), int(b or a)+1))
print(' '.join(map(str,out)))")
fi

MLOAD="$ROOT/bench/mload/mload"
if [ ! -x "$MLOAD" ] || [ -n "$(find "$ROOT/bench/mload" -name '*.go' -newer "$MLOAD")" ]; then
  log "building mload"
  (cd "$ROOT/bench/mload" && go build -o mload .)
fi
command -v jq >/dev/null || die "jq not found"
command -v python3 >/dev/null || die "python3 not found"
[ -f "$ROOT/seed/tokens.json" ] || die "missing seed/tokens.json (run: bash seed/make-seed.sh)"

log "output:  $OUT"
log "targets: $(echo "$TARGETS" | tr ',' '\n' | wc -l | tr -d ' ') IPs x ports $PORTS ($OS, $NCPU cores, $MEM_GIB GiB)"
[ "$OS" = Linux ] && log "pinning: server cpus $SERVER_CPUS (own scope), mload cpus $MLOAD_CPUS, postgres $(cat "$PG_CG/cpuset.cpus" 2>/dev/null || echo '?'), everything else $OTHER_CPUS"
log "building server"
bash "$SUB/build.sh" >>"$OUT/capacity.log" 2>&1

export DATABASE_URL JWT_SECRET=million-users-challenge
export HOST=0.0.0.0 PORT=3000 EXTRA_PORT=3001 DEBUG_PORT=6060 GOMEMLIMIT=${GOMEMLIMIT:-12GiB}
{ echo "os=$OS ncpu=$NCPU mem_gib=$MEM_GIB server_cpus=$([ "$OS" = Linux ] && echo "$SERVER_CPUS")"
  echo "steps=$STEPS step_rate=$STEP_RATE settle=$SETTLE measure=$MEASURE hot_users=$HOT_USERS hot_rps=$HOT_RPS"
  echo "hot_ramp=$HOT_RAMP hot_hold=$HOT_HOLD ladder_rps=$LADDER_RPS base_users=$BASE_USERS"
  env | grep -E '^(DB_POOL|READ_WORKERS|WRITE_WORKERS|MAX_BATCH|GROUP_COMMIT|GOMEMLIMIT|GOGC|GOMAXPROCS|IDLE_TIMEOUT|KEEP_ALIVE_TIMEOUT|READ_TIMEOUT)=' | sort
} > "$OUT/env.txt" || true

# --- process bookkeeping -------------------------------------------------------------------------
SPID="" FPID="" SAMPLERS="" MPIDS=""
stop_phase() {
  for p in $MPIDS $SAMPLERS $SPID $FPID; do kill "$p" 2>/dev/null || true; done
  pkill -f "$MLOAD -target" 2>/dev/null || true
  # A server that hangs on shutdown would block the wait below forever: give it 20 s, then SIGQUIT
  # (goroutine dump into server.log) and finally SIGKILL.
  if [ -n "$FPID" ]; then
    for _ in $(seq 20); do kill -0 "$FPID" 2>/dev/null || break; sleep 1; done
    if kill -0 "$FPID" 2>/dev/null; then
      log "  server did not exit within 20s of SIGTERM; SIGQUIT (goroutine dump in $PH/server.log)"
      kill -QUIT "$FPID" 2>/dev/null || true
      sleep 3
      kill -KILL "$FPID" 2>/dev/null || true
    fi
  fi
  wait 2>/dev/null || true
  SPID="" FPID="" SAMPLERS="" MPIDS=""
}
trap stop_phase EXIT
trap 'log "interrupted"; exit 130' INT TERM

# cpu_s <pid...>: total user+system CPU seconds of the given processes
if [ "$OS" = Darwin ]; then
  cpu_s() {
    [ $# -gt 0 ] || { echo 0; return; }
    ps -o time= -p "$(echo "$@" | tr ' ' ',')" 2>/dev/null \
      | awk '{n=split($1,a,":"); s=0; for(i=1;i<=n;i++) s=s*60+a[i]; tot+=s} END{print tot+0}'
  }
else
  TCK=$(getconf CLK_TCK)
  cpu_s() {
    local p tot=0
    for p in "$@"; do
      # fields after the ")" of comm: utime is field 12, stime 13 (14/15 of the full line)
      tot=$(( tot + $(sed 's/.*) //' "/proc/$p/stat" 2>/dev/null | awk '{print $12 + $13}' || echo 0) ))
    done
    awk -v t="$tot" -v k="$TCK" 'BEGIN{printf "%.2f", t / k}'
  }
  # busy and softirq seconds summed over the server's cpus (includes kernel network work done there)
  srv_box_s() {
    awk -v cpus=" $SRV_CPU_LIST " -v k="$TCK" '
      $1 ~ /^cpu[0-9]+$/ { n = substr($1, 4); if (index(cpus, " " n " ")) { busy += $2+$3+$4+$7+$8+$9; sirq += $8 } }
      END { printf "%.2f,%.2f", busy / k, sirq / k }' /proc/stat
  }
fi

# start_phase <name>: fresh DB + server + samplers, writing into $OUT/<name>/
start_phase() {
  PH="$OUT/$1"
  mkdir -p "$PH"
  log "[$1] resetting database"
  if [ "$OS" = Linux ]; then
    # as box/reset-db.sh: fresh copy, then Postgres restarted with cold caches
    psql -qX -d postgres -c "DROP DATABASE IF EXISTS feed WITH (FORCE)" -c "CREATE DATABASE feed OWNER feed TEMPLATE feed_seed" >/dev/null
    systemctl stop "$PG_UNIT"; sync; echo 3 > /proc/sys/vm/drop_caches; systemctl start "$PG_UNIT"
  else
    psql -qX -d postgres -c "DROP DATABASE IF EXISTS feed WITH (FORCE)" -c "CREATE DATABASE feed TEMPLATE feed_seed" >/dev/null
  fi
  pin_server bash "$SUB/start.sh" >"$PH/server.log" 2>&1 &
  SPID=$!
  local up=""
  for _ in $(seq 60); do
    kill -0 "$SPID" 2>/dev/null || { tail -20 "$PH/server.log" >&2; die "server exited during startup"; }
    if curl -sf -m 2 http://127.0.0.1:3000/health >/dev/null && curl -sf -m 2 http://127.0.0.1:3001/health >/dev/null \
       && curl -sf -m 2 "$DBG/debug/vars" | jq -e '.runtime' >/dev/null; then up=1; break; fi
    sleep 1
  done
  [ -n "$up" ] || { tail -20 "$PH/server.log" >&2; die "server not healthy after 60s (or /debug/vars has no runtime)"; }
  FPID=$(pgrep -f "$SUB/bin/feed" | head -1)
  log "[$1] server up (pid $FPID)"
  echo "name,t0,t1" > "$PH/windows.csv"
  echo "id,start_ms,users,offset,think,ramp_s" > "$PH/mloads.csv"

  # every 2 s: server CPU seconds + RSS, mload CPU seconds, Go runtime metrics; on Linux also Postgres CPU
  # (cgroup), busy/softirq seconds on the server's cpus, kernel slab + TCP buffer memory, mload RSS.
  # set +e: a failed probe (no mload running yet, a slow curl) must skip a column, not kill the sampler
  ( set +eo pipefail
    echo "t,srv_cpu_s,srv_rss_kb,mload_cpu_s,live,goal,stacks,total,released,goroutines,gc,pg_cpu_s,box_srv_s,box_srv_sirq_s,slab_kb,tcpmem_pages,memavail_kb,mload_rss_kb"
    while kill -0 "$FPID" 2>/dev/null; do
      t=$(now)
      srv="$(cpu_s "$FPID"),$(ps -o rss= -p "$FPID" | tr -d ' ')"
      mpids=$(pgrep -f "$MLOAD -target" | tr '\n' ' ')
      ml=$(cpu_s $mpids)
      rt=$(curl -sf -m 2 "$DBG/debug/vars" | jq -r '.runtime | [."/gc/heap/live:bytes", ."/gc/heap/goal:bytes",
            ."/memory/classes/heap/stacks:bytes", ."/memory/classes/total:bytes", ."/memory/classes/heap/released:bytes",
            ."/sched/goroutines:goroutines", ."/gc/cycles/total:gc-cycles"] | @csv' || echo ",,,,,,")
      if [ "$OS" = Linux ]; then
        pg=$(awk '/^usage_usec/{printf "%.2f", $2 / 1e6}' "$PG_CG/cpu.stat")
        box=$(srv_box_s)
        slab=$(awk '/^Slab:/{print $2}' /proc/meminfo)
        tcpm=$(awk '/^TCP:/{for(i=1;i<NF;i++) if ($i=="mem") print $(i+1)}' /proc/net/sockstat)
        avail=$(awk '/^MemAvailable:/{print $2}' /proc/meminfo)
        mrss=$( [ -n "$mpids" ] && ps -o rss= -p "$(echo $mpids | tr ' ' ',')" | awk '{s+=$1} END{print s+0}' || echo 0)
        extra="$pg,$box,$slab,$tcpm,$avail,$mrss"
      else
        extra=",,,,,,,"
      fi
      echo "$t,$srv,$ml,$rt,$extra"
      sleep 2
    done ) > "$PH/samples.csv" &
  SAMPLERS="$!"
  if [ "$OS" = Darwin ]; then
    # postgres container CPU (docker stats takes ~2 s per call, so its own loop)
    ( set +eo pipefail
      echo "t,pg_cpu_pct"
      while kill -0 "$FPID" 2>/dev/null; do
        echo "$(now),$(docker stats --no-stream --format '{{.CPUPerc}}' "$PG_CONTAINER" | tr -d '%')"
      done ) > "$PH/pg.csv" &
    SAMPLERS="$SAMPLERS $!"
  fi
}

# run_mload <id> <users> <offset> <think> <ramp_s> <hold_s>: background mload, start time recorded
run_mload() {
  local start_ms=$(( ($(now) + 2) * 1000 ))
  echo "$1,$start_ms,$2,$3,$4,$5" >> "$PH/mloads.csv"
  pin_mload "$MLOAD" -target "$TARGETS" -ports "$PORTS" -users "$2" -offset "$3" -think-scale "$4" \
    -ramp "${5}s" -hold "${6}s" -start "$start_ms" -gzip -tokens "$ROOT/seed/tokens.json" \
    -out "$PH/$1.json" > "$PH/$1.log" 2>&1 &
  MPIDS="$MPIDS $!"
  sleep 2
}

# measure <name> <seconds>: record a measurement window that ends now + seconds
measure() {
  local t0; t0=$(now)
  local half=$(( $2 / 2 )) prof=""
  # ALLOCS=1: allocation profile at both ends of the window; diff them with pprof -base
  [ -n "${ALLOCS:-}" ] && curl -sf -m 30 -o "$PH/allocs-start-$1.pb.gz" "$DBG/debug/pprof/allocs" || true
  # PROFILE=1: a CPU profile over most of the window (the curl runs off the server's cpus)
  if [ -n "${PROFILE:-}" ]; then
    curl -sf -m $(( $2 + 10 )) -o "$PH/cpu-$1.pb.gz" "$DBG/debug/pprof/profile?seconds=$(( $2 - 4 ))" &
    prof=$!
  fi
  sleep "$half"
  # Mid-window snapshots: where server goroutines are parked, and what Postgres backends are doing.
  curl -sf -m 10 "$DBG/debug/pprof/goroutine?debug=1" > "$PH/goroutines-$1.txt" || true
  psql -qXAt -d feed -c "SELECT coalesce(wait_event_type,'-')||'/'||coalesce(wait_event,'-')||' '||state, count(*)
    FROM pg_stat_activity WHERE datname = 'feed' GROUP BY 1 ORDER BY 2 DESC" > "$PH/pg-activity-$1.txt" 2>&1 || true
  sleep $(( $2 - half ))
  [ -n "$prof" ] && { wait "$prof" 2>/dev/null || true; }
  [ -n "${ALLOCS:-}" ] && curl -sf -m 30 -o "$PH/allocs-end-$1.pb.gz" "$DBG/debug/pprof/allocs" || true
  echo "$1,$t0,$(now)" >> "$PH/windows.csv"
  log "    window $1 measured (${2}s)"
}

ceil_div() { echo $(( ($1 + $2 - 1) / $2 )); }
think_for() { python3 -c "print(round($HOT_USERS * $SCORE_THINK / (23.6 * $1), 4))"; }

# --- phases ----------------------------------------------------------------------------------------
phase_stairs() {
  start_phase stairs
  IFS=, read -ra steps <<< "$STEPS"
  local prev=0 total_s=0 i=0
  for s in "${steps[@]}"; do
    total_s=$(( total_s + $(ceil_div $((s - prev)) "$STEP_RATE") + SETTLE + 5 ))
    prev=$s
  done
  prev=0
  for s in "${steps[@]}"; do
    i=$((i + 1))
    local add=$((s - prev)) ramp; ramp=$(ceil_div "$add" "$STEP_RATE")
    log "[stairs] step $i: +$add users -> $s (ramp ${ramp}s, settle ${SETTLE}s)"
    # each step's mload holds to the end of the staircase; they're killed together afterwards
    run_mload "step$i" "$add" "$prev" "$SCORE_THINK" "$ramp" "$total_s"
    sleep $((ramp + SETTLE - MEASURE))
    measure "users_$s" "$MEASURE"
    prev=$s
  done
  stop_phase
}

# phase_hot [phase name] [target req/s]
phase_hot() {
  local name=${1:-hot} rps=${2:-$HOT_RPS}
  start_phase "$name"
  local think; think=$(think_for "$rps")
  log "[$name] $HOT_USERS users at think-scale $think (aiming for ~$rps req/s)"
  run_mload hot "$HOT_USERS" 0 "$think" "$HOT_RAMP" "$HOT_HOLD"
  sleep $((HOT_RAMP + 30))
  measure "hot_${HOT_USERS}@${rps}" $((HOT_HOLD - 40))
  wait $MPIDS 2>/dev/null || true
  stop_phase
}

phase_ladder() {
  IFS=, read -ra levels <<< "$LADDER_RPS"
  for r in "${levels[@]}"; do phase_hot "ladder_$r" "$r"; done
}

phase_mixed() {
  start_phase mixed
  local ramp think; ramp=$(ceil_div "$BASE_USERS" "$STEP_RATE"); think=$(think_for "$HOT_RPS")
  local hot_total=$((HOT_RAMP + HOT_HOLD))
  log "[mixed] base: $BASE_USERS users at think-scale $SCORE_THINK (ramp ${ramp}s)"
  run_mload base "$BASE_USERS" 0 "$SCORE_THINK" "$ramp" $((SETTLE + hot_total + 10))
  sleep $((ramp + SETTLE - MEASURE))
  measure "base_${BASE_USERS}" "$MEASURE"
  log "[mixed] hot on top: $HOT_USERS users at think-scale $think (~$HOT_RPS req/s)"
  run_mload hot "$HOT_USERS" "$BASE_USERS" "$think" "$HOT_RAMP" "$HOT_HOLD"
  sleep $((HOT_RAMP + 30))
  measure "base_${BASE_USERS}+hot_${HOT_USERS}@${HOT_RPS}" $((HOT_HOLD - 40))
  stop_phase
}

case "$MODE" in
  stairs) phase_stairs ;;
  hot) phase_hot ;;
  ladder) phase_ladder ;;
  mixed) phase_mixed ;;
  all) phase_stairs; phase_hot; phase_mixed ;;
  *) die "usage: $0 [stairs|hot|ladder|mixed|all]" ;;
esac

log "analysing"
python3 "$SUB/bench/capacity_report.py" "$OUT" | tee "$OUT/report.txt"
