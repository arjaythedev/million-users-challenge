#!/usr/bin/env bash
# Are the hot queries really prepared? Log every statement Postgres executes for ~20 s of light load, then count
# them by statement name: "execute _auto1" = auto-prepared, "execute <unnamed>" = parsed each time.
#   check-prepared.sh <label> [SUBMISSION_DIR] [ENV=VAL ...]
set -u
L=$1; shift
DIR=/opt/muc-aspnet/current
if [ $# -gt 0 ] && [ -d "$1" ]; then DIR=$1; shift; fi
OUT=/opt/muc-aspnet/runs/$L; rm -rf "$OUT"; mkdir -p "$OUT"
systemctl stop mua-srv 2>/dev/null; systemctl reset-failed mua-srv 2>/dev/null
sudo -u postgres psql -qX -c "DROP DATABASE IF EXISTS feed WITH (FORCE)" -c "CREATE DATABASE feed OWNER feed TEMPLATE feed_seed" >/dev/null
envs=(--setenv=DATABASE_URL=postgres://feed:feed@127.0.0.1:5432/feed --setenv=JWT_SECRET=million-users-challenge
      --setenv=HOST=0.0.0.0 --setenv=PORT=80 --setenv=EXTRA_PORT=81)
for kv in "$@"; do envs+=(--setenv="$kv"); done
systemd-run -q --unit=mua-srv -p LogRateLimitIntervalSec=30s -p LogRateLimitBurst=200 -p User=challenge -p AllowedCPUs=0-3 -p LimitNOFILE=8388608 \
  -p AmbientCapabilities=CAP_NET_BIND_SERVICE -p WorkingDirectory=$DIR "${envs[@]}" /bin/bash $DIR/start.sh
for i in $(seq 120); do curl -sf -m 1 http://127.0.0.1:80/health >/dev/null && break; sleep 0.5; done
LOG=/var/log/postgresql/postgresql-18-main.log
taskset -c 4-7 /opt/muc-aspnet/mload -target 127.0.0.1 -ports 80,81 -users 2000 -ramp 5s -hold 40s -gzip -think-scale 0.25 \
  -tokens /opt/muc-aspnet/tokens.json -out "$OUT/mload.json" > "$OUT/mload.log" 2>&1 &
sleep 20   # past warm-up: auto-prepare kicks in after 2 uses of a statement on a connection
before=$(wc -l < $LOG)
sudo -u postgres psql -qX -c "ALTER SYSTEM SET log_statement = 'all'" -c "SELECT pg_reload_conf()" >/dev/null
sleep 10
sudo -u postgres psql -qX -c "ALTER SYSTEM RESET log_statement" -c "SELECT pg_reload_conf()" >/dev/null
wait
tail -n +$((before + 1)) $LOG > "$OUT/pg.log"
echo "$L: statements logged in 10 s, by kind:" | tee "$OUT/result.txt"
grep -oE 'LOG:  (execute [^:]*|statement|parse [^:]*|bind [^:]*)' "$OUT/pg.log" | sort | uniq -c | sort -rn | head -12 | tee -a "$OUT/result.txt"
echo "sample:" | tee -a "$OUT/result.txt"; grep -m3 'LOG:  execute' "$OUT/pg.log" | cut -c1-220 | tee -a "$OUT/result.txt"
tail -1 "$OUT/mload.log" | tee -a "$OUT/result.txt"
gzip -f "$OUT/pg.log"
systemctl stop mua-srv
