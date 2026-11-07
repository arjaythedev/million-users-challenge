#!/usr/bin/env bash
# Scale hold on the Synteq box: the server on CPUs 0-3 (transient unit as user challenge, challenge.service limits),
# Postgres and `hold` on CPUs 4-7. `hold` opens N keep-alive connections at RATE/s (the grader ramps at <= 3,300/s),
# each GETs /feed or /posts/:id every ~K s; latency counted after the ramp (WARM s).
#   scale.sh <label> <N> <K> <hold-secs> [SUBMISSION_DIR] [ENV=VAL ...]
# Result: /opt/muc/runs/<label>/result.txt
set -u
L=$1; N=$2; K=$3; HOLDS=$4; shift 4
DIR=/opt/muc/current
if [ $# -gt 0 ] && [ -d "$1" ]; then DIR=$1; shift; fi
OUT=/opt/muc/runs/$L; rm -rf "$OUT"; mkdir -p "$OUT"
SRV_CPUS=${SRV_CPUS:-0-3}; OTHER_CPUS=${OTHER_CPUS:-4-7}; RATE=${RATE:-3300}
systemctl stop muc-srv 2>/dev/null; sleep 1; systemctl reset-failed muc-srv 2>/dev/null; pkill -x hold; pkill -x idle; pkill -x mload
for i in $(seq 30); do systemctl is-active -q muc-srv || break; sleep 1; done
systemctl set-property --runtime postgresql@18-main.service AllowedCPUs=$OTHER_CPUS
systemctl restart postgresql@18-main
sudo -u postgres psql -qX -c "DROP DATABASE IF EXISTS feed WITH (FORCE)" -c "CREATE DATABASE feed OWNER feed TEMPLATE feed_seed" >/dev/null
systemctl stop postgresql@18-main; sync; echo 3 > /proc/sys/vm/drop_caches; systemctl start postgresql@18-main
envs=(--setenv=DATABASE_URL=postgres://feed:feed@127.0.0.1:5432/feed --setenv=JWT_SECRET=million-users-challenge
      --setenv=HOST=0.0.0.0 --setenv=PORT=80 --setenv=EXTRA_PORT=81)
for kv in "$@"; do envs+=(--setenv="$kv"); done
echo "$L dir=$DIR n=$N k=$K hold=$HOLDS rate=$RATE env=$*" > "$OUT/args"
systemd-run -q --unit=muc-srv -p User=challenge -p AllowedCPUs=$SRV_CPUS -p LimitNOFILE=8388608 \
  -p AmbientCapabilities=CAP_NET_BIND_SERVICE -p WorkingDirectory=$DIR "${envs[@]}" /bin/bash $DIR/start.sh
for i in $(seq 120); do curl -sf -m 1 http://127.0.0.1:80/health >/dev/null && break; sleep 0.5; done
PID=$(systemctl show -p MainPID --value muc-srv)
RAMP=$(( N / RATE + 1 )); WARM=$(( RAMP + 30 )); SECS=$(( WARM + HOLDS ))
nstat -n >/dev/null
taskset -c $OTHER_CPUS /opt/muc/hold -n $N -ips 2-17 -ports 80,81 -rate $RATE -k $K -secs $SECS -warm $WARM -to 10 > "$OUT/hold.log" 2>&1 &
HP=$!
mon() { # every 10 s: server rss, server cgroup cpu usec, TCP mem pages, slab kB
  while kill -0 $HP 2>/dev/null; do
    echo "$(date +%s) rss=$(awk '/VmRSS/{print $2}' /proc/$PID/status 2>/dev/null) cpu=$(awk '/usage_usec/{print $2}' /sys/fs/cgroup/system.slice/muc-srv.service/cpu.stat) $(grep TCP: /proc/net/sockstat | tr ' ' '_') slab=$(awk '/^Slab:/{print $2}' /proc/meminfo) free=$(awk '/MemAvailable/{print $2}' /proc/meminfo)"
    sleep 10
  done > "$OUT/mon.log"
}
mon &
wait $HP
nstat > "$OUT/nstat.txt"
journalctl -q _SYSTEMD_INVOCATION_ID="$(systemctl show -p InvocationID --value muc-srv)" --no-pager -o cat | tail -200 > "$OUT/server.log"
alive=$(systemctl is-active muc-srv)
{
  cat "$OUT/args"
  echo "server: $alive  $(tail -1 "$OUT/mon.log")"
  grep TOTAL "$OUT/hold.log"
  awk -v w=$WARM '/^t=/{t=substr($1,3)+0; if (t>w) {for(i=2;i<=NF;i++) if ($i ~ /^p99=/) {v=substr($i,5)+0; if (v>m) m=v}}} END{print "worst 10s-window p99 after ramp: " m " ms"}' "$OUT/hold.log"
  grep -E "TcpRetransSegs|TCPLossProbes|TCPTimeouts|ListenOverflows|ListenDrops|TCPAbortOnMemory|TCPMemoryPressures|PruneCalled" "$OUT/nstat.txt" | awk '{printf "%s=%s ", $1, $2} END{print ""}'
} | tee "$OUT/result.txt"
systemctl stop muc-srv; true
