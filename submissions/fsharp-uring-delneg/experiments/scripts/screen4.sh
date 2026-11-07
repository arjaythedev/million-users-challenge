#!/usr/bin/env bash
# One screen on the Synteq box (8 vCPUs): the server on CPUs 0-3 (a transient systemd unit as user challenge with
# challenge.service's limits), Postgres, mload and the idle holder on CPUs 4-7. Fresh database + cold caches first.
#   screen.sh <label> <users> <think-scale> <idle-conns> [SUBMISSION_DIR] [ENV=VAL ...]
# screen2: + per-direction packets/bytes per request via tc prio bands on lo, LOSS=<pct> netem loss on both
# app directions (sport 80/81 = server->client band 3, dport 80/81 = client->server band 4).
# Window: mload starts (ramp 30 s, hold 120 s); CPU is read at +60 s and +150 s. Result: runs/<label>/result.txt
set -u
L=$1; U=$2; TS=$3; IDLE=$4; shift 4
DIR=/opt/muc/current
if [ $# -gt 0 ] && [ -d "$1" ]; then DIR=$1; shift; fi
OUT=/opt/muc/runs/$L; rm -rf "$OUT"; mkdir -p "$OUT"
SRV_CPUS=${SRV_CPUS:-0-3}; OTHER_CPUS=${OTHER_CPUS:-4-7}
# screen4: HOLD_S= sets mload's hold (default 120 s; 300 = a grader-length hold)
# screen3: Postgres and the load on CPUs of their own (PG_CPUS, LOAD_CPUS), so a dispatch that wakes Postgres more
# isn't charged for starving it (the grader's database box has 16 cores to itself)
PG_CPUS=${PG_CPUS:-$OTHER_CPUS}; LOAD_CPUS=${LOAD_CPUS:-$OTHER_CPUS}
systemctl stop muc-srv 2>/dev/null; systemctl reset-failed muc-srv 2>/dev/null; pkill -x idle 2>/dev/null; pkill -x mload 2>/dev/null
systemctl set-property --runtime postgresql@18-main.service AllowedCPUs=$PG_CPUS
systemctl restart postgresql@18-main
sudo -u postgres psql -qX -c "DROP DATABASE IF EXISTS feed WITH (FORCE)" -c "CREATE DATABASE feed OWNER feed TEMPLATE feed_seed" >/dev/null
systemctl stop postgresql@18-main; sync; echo 3 > /proc/sys/vm/drop_caches; systemctl start postgresql@18-main
envs=(--setenv=DATABASE_URL=postgres://feed:feed@127.0.0.1:5432/feed --setenv=JWT_SECRET=million-users-challenge
      --setenv=HOST=0.0.0.0 --setenv=PORT=80 --setenv=EXTRA_PORT=81 --setenv=MUC_STATS=1)
for kv in "$@"; do envs+=(--setenv="$kv"); done
[ -n "${PERF:-}" ] && envs+=(--setenv=DOTNET_PerfMapEnabled=1 --setenv=DOTNET_EnableWriteXorExecute=0)
LOSS=${LOSS:-0}
tc qdisc del dev lo root 2>/dev/null
LO_MTU=${LO_MTU-1500}  # 1500 like the grader NIC (segment counts); LO_MTU= keeps 65536
if [ -n "${LO_MTU:-}" ]; then ip link set lo mtu "$LO_MTU"; fi
tc qdisc add dev lo root handle 1: prio bands 6 priomap 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
NETEM_S2C=${NETEM_S2C:-loss ${LOSS}% limit 1000000}; NETEM_C2S=${NETEM_C2S:-loss ${LOSS}% limit 1000000}
tc qdisc add dev lo parent 1:3 handle 30: netem $NETEM_S2C
tc qdisc add dev lo parent 1:4 handle 40: netem $NETEM_C2S
NETEM_PG=${NETEM_PG:-limit 1000000}
tc qdisc add dev lo parent 1:5 handle 50: netem $NETEM_PG
tc qdisc add dev lo parent 1:6 handle 60: netem $NETEM_PG
tc filter add dev lo parent 1:0 protocol ip prio 1 u32 match ip sport 5432 0xffff flowid 1:5
tc filter add dev lo parent 1:0 protocol ip prio 1 u32 match ip dport 5432 0xffff flowid 1:6
for pt in 80 81; do
  tc filter add dev lo parent 1:0 protocol ip prio 1 u32 match ip sport $pt 0xffff flowid 1:3
  tc filter add dev lo parent 1:0 protocol ip prio 1 u32 match ip dport $pt 0xffff flowid 1:4
done
echo "LO_MTU=${LO_MTU:-} PG=[$NETEM_PG] PGHICCUP=${PGHICCUP:-} S2C=[$NETEM_S2C] C2S=[$NETEM_C2S] $L dir=$DIR users=$U ts=$TS idle=$IDLE env=$*" > "$OUT/args"
systemd-run -q --unit=muc-srv -p User=challenge -p AllowedCPUs=$SRV_CPUS -p LimitNOFILE=8388608 \
  -p AmbientCapabilities=CAP_NET_BIND_SERVICE -p WorkingDirectory=$DIR "${envs[@]}" /bin/bash $DIR/start.sh
for i in $(seq 120); do curl -sf -m 1 http://127.0.0.1:80/health >/dev/null && break; sleep 0.5; done
PID=$(systemctl show -p MainPID --value muc-srv)
if [ "$IDLE" -gt 0 ]; then
  taskset -c $LOAD_CPUS /opt/muc/idle -n "$IDLE" -ips 2-9 -ports 80,81 -rate 10000 > "$OUT/idle.log" 2>&1 &
  for i in $(seq 120); do sleep 1; o=$(tail -1 "$OUT/idle.log" | sed -n 's/.*resp=\([0-9]*\).*/\1/p'); [ "${o:-0}" -ge "$IDLE" ] && break; done
fi
snap() { # cpu0-3 busy/steal ticks, server cgroup usec, postgres cgroup usec, rss kB, sockstat, slab
  awk -v c="$SRV_CPUS" 'BEGIN{split(c,r,"-")} $1 ~ /^cpu[0-9]+$/ {n=substr($1,4)+0; if(n>=r[1]&&n<=r[2]){b+=$2+$3+$4+$7+$8; s+=$7+$8; st+=$9; id+=$5+$6}} END{print b, s, st, id}' /proc/stat
  awk '/usage_usec|user_usec|system_usec/{printf "%s ", $2} END{print ""}' /sys/fs/cgroup/system.slice/muc-srv.service/cpu.stat
  awk '/usage_usec/{print $2}' /sys/fs/cgroup/system.slice/system-postgresql.slice/postgresql@18-main.service/cpu.stat
  awk '/VmRSS/{print $2}' /proc/$PID/status
  grep TCP: /proc/net/sockstat
  awk '/^Slab:/{print $2}' /proc/meminfo
  tc -s qdisc show dev lo | awk '/qdisc netem 30:/{f=1} /qdisc netem 40:/{f=2} /qdisc netem 50:/{f=3} /qdisc netem 60:/{f=4} /Sent/{if(f){print f, $2, $4, $7; f=0}}' | tr '\n' ' '; echo
  nstat -az TcpExtTCPLossProbes TcpRetransSegs TcpExtDelayedACKs TcpExtTCPTimeouts 2>/dev/null | awk 'NR>1{printf "%s=%s ", $1, $2} END{print ""}'
}
taskset -c $LOAD_CPUS /opt/muc/mload -target 127.0.0.1 -ports 80,81 -users "$U" -ramp 30s -hold ${HOLD_S:-120}s -gzip -think-scale "$TS" \
  -tokens /opt/muc/tokens.json -out "$OUT/mload.json" > "$OUT/mload.log" 2>&1 &
MP=$!
HP=""
if [ -n "${PGHICCUP:-}" ]; then # Postgres hiccups: CHECKPOINT + VACUUM ANALYZE every PGHICCUP s during the run
  ( sleep 40; while kill -0 $MP 2>/dev/null; do
      taskset -c $LOAD_CPUS sudo -u postgres psql -d feed -qX -c "CHECKPOINT" -c "VACUUM ANALYZE likes" -c "VACUUM ANALYZE posts" >/dev/null 2>&1
      sleep "$PGHICCUP"; done ) &
  HP=$!
fi
sleep 60; snap > "$OUT/snap0"
if [ -n "${PERF:-}" ]; then # profile the server's CPUs for 20 s (perturbs the CPU numbers: a separate run)
  perf record -q -C $SRV_CPUS -g -o "$OUT/perf.data" -- sleep 20 2>/dev/null
  perf report -q -i "$OUT/perf.data" --no-children --sort comm,dso,sym --percent-limit 0.4 2>/dev/null | grep -v "^$" | grep -v "^#" | grep "%" | head -80 > "$OUT/perf.txt"
  perf report -q -i "$OUT/perf.data" --no-children --sort dso --percent-limit 0.5 2>/dev/null | grep "%" | head -20 > "$OUT/perf-dso.txt"
  rm -f "$OUT/perf.data"; sleep 70
else # instructions and cycles on the server's CPUs: steadier than time on a shared-vCPU host
  # and Postgres's instructions and cycles (its cgroup, all CPUs): its cost per request, steadier than its CPU time
  perf stat -x, -e instructions,cycles -a -G system.slice/system-postgresql.slice -o "$OUT/pgstat.csv" -- sleep 90 &
  perf stat -x, -e instructions:u,instructions:k,cycles:u,cycles:k -C $SRV_CPUS -o "$OUT/pstat.csv" -- sleep 90
  wait $!
fi
snap > "$OUT/snap1"
wait $MP
[ -n "$HP" ] && kill $HP 2>/dev/null
journalctl -q _SYSTEMD_INVOCATION_ID="$(systemctl show -p InvocationID --value muc-srv)" --no-pager -o cat > "$OUT/server.log"
python3 - "$OUT" <<'PY' | tee "$OUT/result.txt"
import sys, re, os
d = sys.argv[1]
a = [l.split() for l in open(d + '/snap0')]; b = [l.split() for l in open(d + '/snap1')]
busy = int(b[0][0]) - int(a[0][0]); soft = int(b[0][1]) - int(a[0][1]); steal = int(b[0][2]) - int(a[0][2]); idle = int(b[0][3]) - int(a[0][3])
cg = [int(x) - int(y) for x, y in zip(b[1], a[1])]
pg = int(b[2][0]) - int(a[2][0])
reqs = 0
for l in open(d + '/mload.log'):
    m = re.match(r't=\s*(\d+)s .*req/s=(\d+)', l)
    if m and 60 < int(m.group(1)) <= 150: reqs += int(m.group(2)) * 5
res = [l.strip() for l in open(d + '/mload.log') if l.startswith('RESULT')]
args = open(d + '/args').read().strip()
tot = busy + idle + steal
print(args)
print('reqs=%d req/s=%.0f  cpu0-3 us/req=%.1f (softirq %.1f) app cgroup us/req=%.1f (user %.1f sys %.1f)  util=%.0f%% steal=%.1f%%  pg us/req=%.1f'
      % (reqs, reqs / 90, busy * 1e4 / max(reqs, 1), soft * 1e4 / max(reqs, 1), cg[0] / max(reqs, 1), cg[1] / max(reqs, 1), cg[2] / max(reqs, 1),
         100 * busy / max(tot, 1), 100 * steal / max(tot, 1), pg / max(reqs, 1)))
print('rss_kB=%s -> %s  %s  slab_kB=%s' % (a[3][0], b[3][0], ' '.join(b[4]), b[5][0]))
print(res[-1] if res else 'NO RESULT')
w = [float(m.group(1)) for l in open(d + '/mload.log') for m in [re.search(r'p99=([0-9.]+)ms', l)] if m and l.startswith('t=') and 40 <= int(re.match(r't=\s*(\d+)', l).group(1)) <= 30 + int(os.environ.get('HOLD_S', '120'))]
print('worst 5s-window p99 (t=40-%d s): %.1f ms' % (30 + int(os.environ.get('HOLD_S', '120')), max(w) if w else -1))
try:
    ta = a[6]; tb = b[6]
    def tcv(t):
        d = {}
        for i in range(0, len(t) - 3, 4): d[t[i]] = (int(t[i+1]), int(t[i+2]), int(t[i+3].rstrip(',')))
        return d
    A = tcv(ta); B = tcv(tb); r = max(reqs, 1)
    s2c = [B['1'][k] - A['1'][k] for k in range(3)]; c2s = [B['2'][k] - A['2'][k] for k in range(3)]
    print('per req: srv->cli pkts=%.3f bytes=%.1f dropped=%.4f | cli->srv pkts=%.3f bytes=%.1f dropped=%.4f' % (
        s2c[1] / r, s2c[0] / r, s2c[2] / r, c2s[1] / r, c2s[0] / r, c2s[2] / r))
    if '3' in B and '3' in A:
        p2s = [B['3'][k] - A['3'][k] for k in range(3)]; s2p = [B['4'][k] - A['4'][k] for k in range(3)]
        print('per req: pg->srv pkts=%.3f bytes=%.1f | srv->pg pkts=%.3f bytes=%.1f | SEGMENTS total=%.3f (app in %.3f / out %.3f)' % (
            p2s[1] / r, p2s[0] / r, s2p[1] / r, s2p[0] / r, (s2c[1] + c2s[1] + p2s[1] + s2p[1]) / r, (c2s[1] + p2s[1]) / r, (s2c[1] + s2p[1]) / r))
    na = dict(x.split('=') for x in a[7]); nb = dict(x.split('=') for x in b[7])
    print('per kreq: ' + ' '.join('%s=%.2f' % (k.replace('TcpExt', ''), (int(nb[k]) - int(na[k])) * 1000 / r) for k in nb))
except Exception as ex:
    print('tc parse failed', ex)
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
systemctl stop muc-srv; pkill -x idle 2>/dev/null; tc qdisc del dev lo root 2>/dev/null; [ -n "${LO_MTU:-}" ] && ip link set lo mtu 65536; true
