#!/usr/bin/env bash
# One measured run on the one-box VPS (perf/setup.sh first), as root:
#
#   bash perf/run.sh USERS [SERVER_CORES=4] [THINK_SCALE=2.5] [RAMP=60s] [HOLD=2m]
#
# A fresh database (box/reset-db.sh), then the server on CPUs 0..SERVER_CORES-1 (taskset, the way 8- and 4-core
# scoring restricts it), Postgres on the next half of the rest and mload on the last half, mload aimed at 8 loopback
# addresses x ports 80 and 81 (16 targets, so ~1M source ports). THINK_SCALE=1000 leaves users idle after their first
# page: that run is for memory per idle connection.
#
# Everything lands in perf/results/<run>/: summary.txt (status PASS / FAIL / INVALID / INCOMPLETE first), mload's
# output and JSON, the server's log and GC log, 5 s samples (server RSS, TCP memory and sockets, slab, MemAvailable,
# CPU), mpstat, pidstat (java, postgres, mload), vmstat, Postgres activity and log, and any OOM kills.
#
# Caveats: over loopback both ends of every connection are on this box, and the kernel's receive work runs on the
# sending CPU, so the server's CPUs are charged some of mload's network work and spared some of their own. The CPU
# figure covers the whole run (ramp, startup, some teardown) with steal excluded, per completed request attempt.
# Memory figures are separate whole-host observations, not bytes per server connection. Use them to compare
# changes and to spot trouble, not as the scoring box's figures.
set -euo pipefail
USERS=${1:?usage: run.sh USERS [SERVER_CORES] [THINK_SCALE] [RAMP] [HOLD]}
CORES=${2:-4}
THINK=${3:-2.5}
RAMP=${4:-60s}
HOLD=${5:-2m}
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
TOKENS="$ROOT/seed/tokens.json"
PGLOG=/var/log/postgresql/postgresql-18-main.log
N=$(nproc)
[[ $USERS =~ ^[0-9]+$ && $USERS -gt 0 && $CORES =~ ^[0-9]+$ && $CORES -gt 0 ]] \
  || { echo "USERS and SERVER_CORES must be positive integers"; exit 2; }
(( N - CORES >= 2 )) || { echo "$N CPUs: SERVER_CORES must leave at least 2 for Postgres and mload"; exit 2; }
PG_N=$(( (N - CORES) / 2 ))
SERVER_CPUS="0-$((CORES - 1))"
PG_CPUS="$CORES-$((CORES + PG_N - 1))"
LOAD_CPUS="$((CORES + PG_N))-$((N - 1))"
OUT="$HERE/perf/results/$(date +%Y%m%d-%H%M%S)-u$USERS-c$CORES-t$THINK"
mkdir -p "$OUT"
echo "status=INCOMPLETE" > "$OUT/summary.txt"
ulimit -n 8388608

SERVER='' SAMPLER='' LOAD=''
MONITORS=()
cleanup() {
  local rc=$?
  trap - EXIT
  set +e
  local pids=()
  for pid in "$LOAD" "$SAMPLER" "${MONITORS[@]}" "$SERVER"; do
    [ -n "$pid" ] && pids+=("$pid")
  done
  if [ ${#pids[@]} -gt 0 ]; then
    kill -TERM "${pids[@]}" 2>/dev/null
    for _ in $(seq 1 50); do
      kill -0 "${pids[@]}" 2>/dev/null || break # true while any is alive
      sleep 0.1
    done
    kill -KILL "${pids[@]}" 2>/dev/null
    wait 2>/dev/null
  fi
  echo "runner_exit=$rc" >> "$OUT/summary.txt"
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

jq -e 'type == "array" and length > 0' "$TOKENS" >/dev/null || { echo "no tokens at $TOKENS: run perf/setup.sh"; exit 1; }
if ss -Hltn '( sport = :80 or sport = :81 )' | grep -q .; then
  echo "ports 80/81 are already in use (a server from an earlier run?):"
  ss -ltnp '( sport = :80 or sport = :81 )'
  exit 1
fi

bash "$ROOT/box/reset-db.sh" >/dev/null
for pid in $(pgrep -u postgres); do taskset -a -p -c "$PG_CPUS" "$pid" >/dev/null; done
PGLOG_LINES=$(wc -l < "$PGLOG" 2>/dev/null || echo 0)
DMESG_LINES=$(dmesg | wc -l)

# CPU jiffies (USER_HZ) of the server's CPUs: work (user nice system irq softirq), and steal on its own.
cpu_sum() { awk -v hi=$((CORES - 1)) -v cols="$1" '$1 ~ /^cpu[0-9]+$/ && substr($1, 4) + 0 <= hi {
  n = split(cols, c, ","); for (i = 1; i <= n; i++) b += $(c[i]) } END { printf "%.0f\n", b }' /proc/stat; }
busy() { cpu_sum 2,3,4,7,8; }
steal() { cpu_sum 9; }
or_na() { local v; v=$("$@" 2>/dev/null) || v=''; echo "${v:-NA}"; }
proc_cpu() { awk '{ print $14 + $15 }' "/proc/$SERVER/stat"; }
rss_kb() { awk '/^VmRSS/ { print $2 }' "/proc/$SERVER/status"; }
tcp_field() { awk -v f="$1" '$1 == "TCP:" { for (i = 2; i < NF; i++) if ($i == f) print $(i + 1) }' /proc/net/sockstat; }
meminfo_kb() { awk -v k="$1:" '$1 == k { print $2 }' /proc/meminfo; }

JAVA_TOOL_OPTIONS="-Xlog:gc*,safepoint:file=$OUT/gc.log:time,uptime,level,tags:filecount=3,filesize=10M" \
DATABASE_URL=postgres://feed:feed@127.0.0.1:5432/feed JWT_SECRET=million-users-challenge \
HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 \
  taskset -c "$SERVER_CPUS" bash "$HERE/start.sh" > "$OUT/server.log" 2>&1 &
SERVER=$!
healthy() { curl -fs --noproxy '*' --connect-timeout 1 --max-time 2 -o /dev/null "http://127.0.0.1:$1/health"; }
DEADLINE=$((SECONDS + 60))
until healthy 80 && healthy 81; do
  kill -0 "$SERVER" 2>/dev/null || { echo "the server exited during startup:"; tail -50 "$OUT/server.log"; exit 1; }
  [ "$SECONDS" -lt "$DEADLINE" ] || { echo "not healthy on 80 and 81 within 60 s:"; tail -50 "$OUT/server.log"; exit 1; }
  sleep 0.2
done
sleep 2

BASE_RSS=$(or_na rss_kb); BASE_TCP=$(or_na tcp_field mem); BASE_SLAB=$(or_na meminfo_kb Slab)
echo "t rss_kb tcp_mem_pages tcp_inuse slab_kb memavailable_kb busy steal" > "$OUT/samples.txt"
(
  while true; do
    echo "$(date +%s) $(or_na rss_kb) $(or_na tcp_field mem) $(or_na tcp_field inuse) $(or_na meminfo_kb Slab)" \
      "$(or_na meminfo_kb MemAvailable) $(busy) $(steal)" >> "$OUT/samples.txt"
    sudo -u postgres psql -qXAt -F' ' -d postgres -c "SELECT now()::time(0), state, wait_event_type, wait_event,
      count(*) FROM pg_stat_activity WHERE datname = 'feed' GROUP BY 1, 2, 3, 4" >> "$OUT/pg_activity.txt" 2>&1 || true
    sleep 5
  done
) &
SAMPLER=$!
mpstat -P ALL 5 > "$OUT/mpstat.txt" 2>&1 &
MONITORS+=("$!")
pidstat -h -u -r -C 'java|postgres|mload' 5 > "$OUT/pidstat.txt" 2>&1 &
MONITORS+=("$!")
vmstat -w 5 > "$OUT/vmstat.txt" 2>&1 &
MONITORS+=("$!")

BUSY0=$(busy); STEAL0=$(steal); PROC0=$(or_na proc_cpu)
TARGETS=$(seq -s, -f '127.0.0.%g' 1 8)
echo "mload: $USERS users, ramp $RAMP, hold $HOLD; progress in $OUT/mload.txt"
taskset -c "$LOAD_CPUS" "$ROOT/bench/mload/mload" -target "$TARGETS" -ports 80,81 -users "$USERS" \
  -ramp "$RAMP" -hold "$HOLD" -gzip -think-scale "$THINK" -tokens "$TOKENS" -out "$OUT/mload.json" \
  > "$OUT/mload.txt" 2>&1 &
LOAD=$!
LOAD_RC=0
wait "$LOAD" || LOAD_RC=$?
LOAD=''
BUSY1=$(busy); STEAL1=$(steal); PROC1=$(or_na proc_cpu)

kill "$SAMPLER" "${MONITORS[@]}" 2>/dev/null || true
tail -n +"$((PGLOG_LINES + 1))" "$PGLOG" > "$OUT/postgres.log" 2>/dev/null || true
dmesg | tail -n +"$((DMESG_LINES + 1))" | grep -i -E 'oom|killed process' > "$OUT/oom.txt" || true

HZ=$(getconf CLK_TCK)
PAGE=$(getconf PAGESIZE)
REQS=$(jq -er '.reqs | select(type == "number" and . > 0)' "$OUT/mload.json" 2>/dev/null) || REQS=''
if [ -z "$REQS" ]; then STATUS=INVALID
elif jq -e '.pass == true' "$OUT/mload.json" >/dev/null; then STATUS=PASS
else STATUS=FAIL; fi
{
  echo "status=$STATUS"
  echo "run: $USERS users, server on $CORES cores (CPUs $SERVER_CPUS), Postgres on $PG_CPUS, mload on $LOAD_CPUS," \
    "think-scale $THINK, ramp $RAMP, hold $HOLD; mload_exit=$LOAD_RC"
  sed -n '/^RESULT/,$p' "$OUT/mload.txt" | head -12
  [ -n "$REQS" ] || { echo "no valid result from mload; its last lines:"; tail -5 "$OUT/mload.txt"; }
  grep 'feed: listening' "$OUT/server.log" || echo "no startup line in server.log"
  if [ -n "$REQS" ]; then
    awk -v b=$((BUSY1 - BUSY0)) -v s=$((STEAL1 - STEAL0)) -v hz="$HZ" -v r="$REQS" 'BEGIN {
      printf "CPU, whole run (ramp, startup, some teardown; loopback; steal excluded): %.1f us per completed request\n", b / hz / r * 1e6
      printf "  on the server CPUs; steal was %.1f%% of their non-idle time; %d requests\n", (b + s > 0 ? s * 100 / (b + s) : 0), r }'
    if [[ $PROC0 =~ ^[0-9]+$ && $PROC1 =~ ^[0-9]+$ ]]; then
      awk -v p=$((PROC1 - PROC0)) -v hz="$HZ" -v r="$REQS" 'BEGIN { printf "  of which in the server process: %.1f us per request\n", p / hz / r * 1e6 }'
    else
      echo "  server process CPU: unavailable (the server had exited)"
    fi
  fi
  awk -v r0="$BASE_RSS" -v t0="$BASE_TCP" -v s0="$BASE_SLAB" -v pg="$PAGE" '
    NR > 1 && NF == 8 && $2 ~ /^[0-9]+$/ && $3 ~ /^[0-9]+$/ && $5 ~ /^[0-9]+$/ && $6 ~ /^[0-9]+$/ {
      if ($2 > r) r = $2; if ($3 > t) t = $3; if ($4 > i) i = $4; if ($5 > s) s = $5
      if (m == "" || $6 < m) m = $6; n++ }
    BEGIN { r = t = i = s = 0; m = "" }
    END {
      if (n == 0) { print "memory: no valid samples"; exit }
      printf "server RSS: baseline %.0f MiB, observed peak %.0f MiB\n", r0 / 1024, r / 1024
      printf "whole host, peak minus before (both ends of every connection, not to be added together):\n"
      printf "  TCP buffers %+.0f MiB, slab %+.0f MiB, peak TCP sockets in use %d, lowest MemAvailable %.1f GiB\n",
        (t - t0) * pg / 1048576, (s - s0) / 1024, i, m / 1048576 }' "$OUT/samples.txt"
  echo "server log: $(grep -c -i -E 'error|exception|errno|down:' "$OUT/server.log" || true) error-like lines; GC log: $OUT/gc.log"
  if [ -s "$OUT/oom.txt" ]; then echo "OOM kills:"; cat "$OUT/oom.txt"; fi
} > "$OUT/summary.txt"
cat "$OUT/summary.txt"
[ "$STATUS" = PASS ]
