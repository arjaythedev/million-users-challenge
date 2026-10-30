#!/usr/bin/env bash
# What the managed heap holds per idle connection: start the server (CPUs 0-3), open N idle connections (one GET /feed
# each, gzip), wait, take a gcdump and print the top types by size, plus RSS and the GC's own numbers.
#   gcdump.sh <label> <N> [SUBMISSION_DIR] [ENV=VAL ...]
set -u
L=$1; N=$2; shift 2
DIR=/opt/muc-aspnet/current
if [ $# -gt 0 ] && [ -d "$1" ]; then DIR=$1; shift; fi
OUT=/opt/muc-aspnet/runs/$L; rm -rf "$OUT"; mkdir -p "$OUT"; chmod 777 "$OUT"
systemctl stop mua-srv 2>/dev/null; systemctl reset-failed mua-srv 2>/dev/null; pkill -x idle 2>/dev/null; sleep 1
envs=(--setenv=DATABASE_URL=postgres://feed:feed@127.0.0.1:5432/feed --setenv=JWT_SECRET=million-users-challenge
      --setenv=HOST=0.0.0.0 --setenv=PORT=80 --setenv=EXTRA_PORT=81)
for kv in "$@"; do envs+=(--setenv="$kv"); done
systemd-run -q --unit=mua-srv -p User=challenge -p AllowedCPUs=0-3 -p LimitNOFILE=8388608 \
  -p AmbientCapabilities=CAP_NET_BIND_SERVICE -p WorkingDirectory=$DIR "${envs[@]}" /bin/bash $DIR/start.sh
for i in $(seq 120); do curl -sf -m 1 http://127.0.0.1:80/health >/dev/null && break; sleep 0.5; done
PID=$(systemctl show -p MainPID --value mua-srv)
taskset -c 4-7 /opt/muc-aspnet/idle -n "$N" -ips 2-17 -ports 80,81 -rate 5000 -path /feed > "$OUT/idle.log" 2>&1 &
for i in $(seq 300); do sleep 1; o=$(tail -1 "$OUT/idle.log" | sed -n 's/.*resp=\([0-9]*\).*/\1/p'); [ "${o:-0}" -ge "$N" ] && break; done
sleep 15
echo "$L conns=$N rss_kB=$(awk '/VmRSS/{print $2}' /proc/$PID/status) $(grep TCP: /proc/net/sockstat)" | tee "$OUT/result.txt"
export DOTNET_ROOT=/usr/local/dotnet DOTNET_ROLL_FORWARD=Major
sudo -u challenge -E /opt/muc-aspnet/tools/dotnet-gcdump collect -p $PID -o "$OUT/heap.gcdump" > "$OUT/collect.log" 2>&1
/opt/muc-aspnet/tools/dotnet-gcdump report "$OUT/heap.gcdump" > "$OUT/report.txt" 2>&1
head -3 "$OUT/report.txt" | tee -a "$OUT/result.txt"
# the report lists per-object size and count: total = size x count, top 30 types by total bytes
awk 'NR > 4 && $2 ~ /^[0-9,]+$/ { b = $1; c = $2; gsub(",", "", b); gsub(",", "", c); t = $0; sub(/^ *[0-9,]+ +[0-9,]+ +/, "", t)
     printf "%12d %9d %s\n", b * c, c, t }' "$OUT/report.txt" | sort -rn | head -30 | tee -a "$OUT/result.txt"
echo "after gcdump (a full GC): rss_kB=$(awk '/VmRSS/{print $2}' /proc/$PID/status)" | tee -a "$OUT/result.txt"
rm -f "$OUT/heap.gcdump"
pkill -x idle; sleep 2; systemctl stop mua-srv
