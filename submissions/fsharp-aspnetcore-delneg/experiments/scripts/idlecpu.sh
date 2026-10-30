#!/usr/bin/env bash
# Server CPU with only idle connections: start the server on CPUs 0-3, open N idle keep-alive connections (one
# GET /nope each), wait, then read the unit's cgroup CPU over 30 s. Prints cores used and RSS.
#   idlecpu.sh <label> <N> [SUBMISSION_DIR] [ENV=VAL ...]
set -u
L=$1; N=$2; shift 2
DIR=/opt/muc-aspnet/current
if [ $# -gt 0 ] && [ -d "$1" ]; then DIR=$1; shift; fi
OUT=/opt/muc-aspnet/runs/$L; rm -rf "$OUT"; mkdir -p "$OUT"
systemctl stop mua-srv 2>/dev/null; systemctl reset-failed mua-srv 2>/dev/null; pkill -x idle 2>/dev/null; sleep 1
envs=(--setenv=DATABASE_URL=postgres://feed:feed@127.0.0.1:5432/feed --setenv=JWT_SECRET=million-users-challenge
      --setenv=HOST=0.0.0.0 --setenv=PORT=80 --setenv=EXTRA_PORT=81)
for kv in "$@"; do envs+=(--setenv="$kv"); done
systemd-run -q --unit=mua-srv -p User=challenge -p AllowedCPUs=0-3 -p LimitNOFILE=8388608 \
  -p AmbientCapabilities=CAP_NET_BIND_SERVICE -p WorkingDirectory=$DIR "${envs[@]}" /bin/bash $DIR/start.sh
for i in $(seq 120); do curl -sf -m 1 http://127.0.0.1:80/health >/dev/null && break; sleep 0.5; done
PID=$(systemctl show -p MainPID --value mua-srv)
taskset -c 4-7 /opt/muc-aspnet/idle -n "$N" -ips 2-17 -ports 80,81 -rate 10000 -path /nope > "$OUT/idle.log" 2>&1 &
for i in $(seq 300); do sleep 1; o=$(tail -1 "$OUT/idle.log" | sed -n 's/.*resp=\([0-9]*\).*/\1/p'); [ "${o:-0}" -ge "$N" ] && break; done
sleep 10
cg=/sys/fs/cgroup/system.slice/mua-srv.service/cpu.stat
a=$(awk '/usage_usec/{print $2}' $cg); sleep 30; b=$(awk '/usage_usec/{print $2}' $cg)
echo "$L conns=$N idle cpu=$(( (b - a) / 30000 ))ms/s rss_kB=$(awk '/VmRSS/{print $2}' /proc/$PID/status) $(grep TCP: /proc/net/sockstat)" | tee "$OUT/result.txt"
pkill -x idle; sleep 2; systemctl stop mua-srv
