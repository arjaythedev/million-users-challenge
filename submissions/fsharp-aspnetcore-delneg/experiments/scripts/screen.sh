#!/usr/bin/env bash
# One screen on the Synteq box (8 vCPUs): the server on CPUs 0-3 (a transient systemd unit as user challenge with
# challenge.service's limits), Postgres, mload and the idle holder on CPUs 4-7. Fresh database + cold caches first.
#   screen.sh <label> <users> <think-scale> <idle-conns> [SUBMISSION_DIR] [ENV=VAL ...]
# HOLD=300s for a longer hold (same window); then IDLE_K=100 makes every idle connection send a GET /nope every
# 100 s, so none hits Kestrel's 130 s keep-alive timeout. SRV_CPUS / OTHER_CPUS move the split (default 0-3 / 4-7).
# Window: mload starts (ramp 20 s, hold 100 s); CPU is read at +40 s and +110 s, so the idle connections (opened just
# before mload) are at most ~120 s old: under Kestrel's 130 s keep-alive. Result: runs/<label>/result.txt
set -u
L=$1; U=$2; TS=$3; IDLE=$4; shift 4
DIR=/opt/muc-aspnet/current
if [ $# -gt 0 ] && [ -d "$1" ]; then DIR=$1; shift; fi
OUT=/opt/muc-aspnet/runs/$L; rm -rf "$OUT"; mkdir -p "$OUT"
SRV_CPUS=${SRV_CPUS:-0-3}; OTHER_CPUS=${OTHER_CPUS:-4-7}
systemctl stop mua-srv 2>/dev/null; systemctl reset-failed mua-srv 2>/dev/null; pkill -x idle 2>/dev/null; pkill -x mload 2>/dev/null
systemctl set-property --runtime postgresql@18-main.service AllowedCPUs=$OTHER_CPUS
systemctl restart postgresql@18-main
sudo -u postgres psql -qX -c "DROP DATABASE IF EXISTS feed WITH (FORCE)" -c "CREATE DATABASE feed OWNER feed TEMPLATE feed_seed" >/dev/null
systemctl stop postgresql@18-main; sync; echo 3 > /proc/sys/vm/drop_caches; systemctl start postgresql@18-main
envs=(--setenv=DATABASE_URL=postgres://feed:feed@127.0.0.1:5432/feed --setenv=JWT_SECRET=million-users-challenge
      --setenv=HOST=0.0.0.0 --setenv=PORT=80 --setenv=EXTRA_PORT=81)
for kv in "$@"; do envs+=(--setenv="$kv"); done
[ -n "${PERF:-}" ] && envs+=(--setenv=DOTNET_PerfMapEnabled=1 --setenv=DOTNET_EnableWriteXorExecute=0)
echo "$L dir=$DIR users=$U ts=$TS idle=$IDLE env=$*" > "$OUT/args"
systemd-run -q --unit=mua-srv -p User=challenge -p AllowedCPUs=$SRV_CPUS -p LimitNOFILE=8388608 \
  -p AmbientCapabilities=CAP_NET_BIND_SERVICE -p WorkingDirectory=$DIR "${envs[@]}" /bin/bash $DIR/start.sh
for i in $(seq 120); do curl -sf -m 1 http://127.0.0.1:80/health >/dev/null && break; sleep 0.5; done
PID=$(systemctl show -p MainPID --value mua-srv)
if [ "$IDLE" -gt 0 ]; then
  taskset -c $OTHER_CPUS /opt/muc-aspnet/idle -n "$IDLE" -ips 2-9 -ports 80,81 -rate 10000 ${IDLE_K:+-k $IDLE_K -path /nope} > "$OUT/idle.log" 2>&1 &
  for i in $(seq 120); do sleep 1; o=$(tail -1 "$OUT/idle.log" | sed -n 's/.*resp=\([0-9]*\).*/\1/p'); [ "${o:-0}" -ge "$IDLE" ] && break; done
fi
snap() { # cpu0-3 busy/steal ticks, server cgroup usec, postgres cgroup usec, rss kB, sockstat, slab
  awk -v c="$SRV_CPUS" 'BEGIN{split(c,r,"-")} $1 ~ /^cpu[0-9]+$/ {n=substr($1,4)+0; if(n>=r[1]&&n<=r[2]){b+=$2+$3+$4+$7+$8; s+=$7+$8; st+=$9; id+=$5+$6}} END{print b, s, st, id}' /proc/stat
  awk '/usage_usec|user_usec|system_usec/{printf "%s ", $2} END{print ""}' /sys/fs/cgroup/system.slice/mua-srv.service/cpu.stat
  awk '/usage_usec/{print $2}' /sys/fs/cgroup/system.slice/system-postgresql.slice/postgresql@18-main.service/cpu.stat
  awk '/VmRSS/{print $2}' /proc/$PID/status
  grep TCP: /proc/net/sockstat
  awk '/^Slab:/{print $2}' /proc/meminfo
}
taskset -c $OTHER_CPUS /opt/muc-aspnet/mload -target 127.0.0.1 -ports 80,81 -users "$U" -ramp 20s -hold ${HOLD:-100s} -gzip -think-scale "$TS" \
  -tokens /opt/muc-aspnet/tokens.json -out "$OUT/mload.json" > "$OUT/mload.log" 2>&1 &
MP=$!
sleep 40; snap > "$OUT/snap0"
if [ -n "${PERF:-}" ]; then # profile the server's CPUs for 20 s (perturbs the CPU numbers: a separate run)
  perf record -q -C $SRV_CPUS -g -o "$OUT/perf.data" -- sleep 20 2>/dev/null
  perf report -q -i "$OUT/perf.data" --no-children --sort comm,dso,sym --percent-limit 0.4 2>/dev/null | grep -v "^$" | grep -v "^#" | grep "%" | head -80 > "$OUT/perf.txt"
  perf report -q -i "$OUT/perf.data" --no-children --sort dso --percent-limit 0.5 2>/dev/null | grep "%" | head -20 > "$OUT/perf-dso.txt"
  rm -f "$OUT/perf.data"; sleep 50
else # instructions and cycles on the server's CPUs: steadier than time on a shared-vCPU host
  # and Postgres's instructions and cycles (its cgroup, all CPUs): its cost per request, steadier than its CPU time
  perf stat -x, -e instructions,cycles -a -G system.slice/system-postgresql.slice -o "$OUT/pgstat.csv" -- sleep 70 &
  perf stat -x, -e instructions:u,instructions:k,cycles:u,cycles:k -C $SRV_CPUS -o "$OUT/pstat.csv" -- sleep 70
  wait $!
fi
snap > "$OUT/snap1"
wait $MP
journalctl -q _SYSTEMD_INVOCATION_ID="$(systemctl show -p InvocationID --value mua-srv)" --no-pager -o cat > "$OUT/server.log"
python3 - "$OUT" <<'PY' | tee "$OUT/result.txt"
import sys, re
d = sys.argv[1]
a = [l.split() for l in open(d + '/snap0')]; b = [l.split() for l in open(d + '/snap1')]
busy = int(b[0][0]) - int(a[0][0]); soft = int(b[0][1]) - int(a[0][1]); steal = int(b[0][2]) - int(a[0][2]); idle = int(b[0][3]) - int(a[0][3])
cg = [int(x) - int(y) for x, y in zip(b[1], a[1])]
pg = int(b[2][0]) - int(a[2][0])
reqs = 0
for l in open(d + '/mload.log'):
    m = re.match(r't=\s*(\d+)s .*req/s=(\d+)', l)
    if m and 40 < int(m.group(1)) <= 110: reqs += int(m.group(2)) * 5
res = [l.strip() for l in open(d + '/mload.log') if l.startswith('RESULT')]
args = open(d + '/args').read().strip()
tot = busy + idle + steal
print(args)
print('reqs=%d req/s=%.0f  cpu0-3 us/req=%.1f (softirq %.1f) app cgroup us/req=%.1f (user %.1f sys %.1f)  util=%.0f%% steal=%.1f%%  pg us/req=%.1f'
      % (reqs, reqs / 70, busy * 1e4 / max(reqs, 1), soft * 1e4 / max(reqs, 1), cg[0] / max(reqs, 1), cg[1] / max(reqs, 1), cg[2] / max(reqs, 1),
         100 * busy / max(tot, 1), 100 * steal / max(tot, 1), pg / max(reqs, 1)))
print('rss_kB=%s -> %s  %s  slab_kB=%s' % (a[3][0], b[3][0], ' '.join(b[4]), b[5][0]))
print(res[-1] if res else 'NO RESULT')
import os
if os.path.exists(d + '/pstat.csv'):
    v = {}
    for l in open(d + '/pstat.csv'):
        f = l.strip().split(',')
        if len(f) > 2 and f[0].replace('.', '').isdigit(): v[f[2]] = float(f[0])
    r = max(reqs, 1)
    print('per req: kinstr user=%.1f kernel=%.1f total=%.1f  kcycles user=%.1f kernel=%.1f total=%.1f' % (
        v.get('instructions:u', 0) / r / 1e3, v.get('instructions:k', 0) / r / 1e3, (v.get('instructions:u', 0) + v.get('instructions:k', 0)) / r / 1e3,
        v.get('cycles:u', 0) / r / 1e3, v.get('cycles:k', 0) / r / 1e3, (v.get('cycles:u', 0) + v.get('cycles:k', 0)) / r / 1e3))
if os.path.exists(d + '/pgstat.csv'):
    v = {}
    for l in open(d + '/pgstat.csv'):
        f = l.strip().split(',')
        if len(f) > 2 and f[0].replace('.', '').isdigit(): v[f[2]] = float(f[0])
    r = max(reqs, 1)
    print('postgres per req: kinstr=%.1f kcycles=%.1f' % (v.get('instructions', 0) / r / 1e3, v.get('cycles', 0) / r / 1e3))
PY
systemctl stop mua-srv; pkill -x idle 2>/dev/null; true
