#!/usr/bin/env bash
# Memory per idle connection: start the server (CPUs 0-3), open idle connections in steps with the idle holder
# (CPUs 4-7), and after each step read the server's RSS, the kernel's TCP socket count and memory, and Slab.
#   mem.sh <label> <step1,step2,...> [SUBMISSION_DIR] [ENV=VAL ...]
set -u
L=$1; STEPS=$2; shift 2
DIR=/opt/muc/current
if [ $# -gt 0 ] && [ -d "$1" ]; then DIR=$1; shift; fi
OUT=/opt/muc/runs/$L; rm -rf "$OUT"; mkdir -p "$OUT"
systemctl stop muc-srv 2>/dev/null; systemctl reset-failed muc-srv 2>/dev/null; pkill -x idle 2>/dev/null; sleep 1
envs=(--setenv=DATABASE_URL=postgres://feed:feed@127.0.0.1:5432/feed --setenv=JWT_SECRET=million-users-challenge
      --setenv=HOST=0.0.0.0 --setenv=PORT=80 --setenv=EXTRA_PORT=81)
for kv in "$@"; do envs+=(--setenv="$kv"); done
systemd-run -q --unit=muc-srv -p User=challenge -p AllowedCPUs=0-3 -p LimitNOFILE=8388608 \
  -p AmbientCapabilities=CAP_NET_BIND_SERVICE -p WorkingDirectory=$DIR "${envs[@]}" /bin/bash $DIR/start.sh
for i in $(seq 120); do curl -sf -m 1 http://127.0.0.1:80/health >/dev/null && break; sleep 0.5; done
PID=$(systemctl show -p MainPID --value muc-srv)
row() { printf "%s conns=%s rss_kB=%s %s slab_kB=%s memfree_kB=%s\n" "$1" "$2" "$(awk '/VmRSS/{print $2}' /proc/$PID/status)" \
  "$(grep TCP: /proc/net/sockstat | tr -s ' ')" "$(awk '/^Slab:/{print $2}' /proc/meminfo)" "$(awk '/^MemAvailable:/{print $2}' /proc/meminfo)"; }
sleep 2; row "$L" 0 | tee "$OUT/result.txt"
MAX=$(echo "$STEPS" | tr ',' '\n' | tail -1)
taskset -c 4-7 /opt/muc/idle -n "$MAX" -ips 2-17 -ports 80,81 -rate 10000 > "$OUT/idle.log" 2>&1 &
for s in $(echo "$STEPS" | tr ',' ' '); do
  low=0
  for i in $(seq 600); do
    sleep 1; o=$(tail -1 "$OUT/idle.log" | sed -n 's/.*resp=\([0-9]*\).*/\1/p'); [ "${o:-0}" -ge "$s" ] && break
    # safety: never let the box run out of memory (Postgres and other work live here)
    if [ "$(awk '/^MemAvailable:/{print $2}' /proc/meminfo)" -lt 1500000 ]; then low=1; pkill -STOP -x idle; break; fi
  done
  # stop opening more while measuring: the holder opens at a fixed rate, so read right away
  row "$L" "$(tail -1 "$OUT/idle.log" | sed -n 's/.*open=\([0-9]*\).*/\1/p')" | tee -a "$OUT/result.txt"
  if [ $low = 1 ]; then echo "stopped: MemAvailable under 1.5 GB" | tee -a "$OUT/result.txt"; break; fi
done
tail -1 "$OUT/idle.log" | tee -a "$OUT/result.txt"
curl -s -o /dev/null -w "after: health %{http_code}\n" http://127.0.0.1:80/health | tee -a "$OUT/result.txt"
pkill -x idle; pkill -CONT -x idle; sleep 3
systemctl stop muc-srv
