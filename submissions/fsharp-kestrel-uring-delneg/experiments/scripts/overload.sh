#!/usr/bin/env bash
# Overload check (task 4.10): the server (a muc-srv unit on SRV_CPUS, default 0-3) gets bursts of connections that
# misbehave, then must still serve: same PID, health 200, a gzipped feed that gunzips, a write that commits.
#   overload.sh [DIR] [ENV=VAL ...]
# Bursts (each from 127.0.0.2-9 on both ports, as fast as the kernel lets us):
#   1. 30,000 connects closed at once (some with RST via SO_LINGER 0)
#   2. 20,000 half requests ("GET /feed HTTP/1.1\r\nHo") then close
#   3. 20,000 full gzip feed requests whose answers are never read, then close (sends hit dead sockets)
#   4. 2,000 pipelined bursts of 64 feed requests each, read slowly (tiny SO_RCVBUF), then close
set -u
DIR=/opt/muc/current
if [ $# -gt 0 ] && [ -d "$1" ]; then DIR=$1; shift; fi
SRV_CPUS=${SRV_CPUS:-0-3}; OTHER_CPUS=${OTHER_CPUS:-4-7}
systemctl stop muc-srv 2>/dev/null; systemctl reset-failed muc-srv 2>/dev/null
sudo -u postgres psql -qX -c "DROP DATABASE IF EXISTS feed WITH (FORCE)" -c "CREATE DATABASE feed OWNER feed TEMPLATE feed_seed" >/dev/null
envs=(--setenv=DATABASE_URL=postgres://feed:feed@127.0.0.1:5432/feed --setenv=JWT_SECRET=million-users-challenge
      --setenv=HOST=0.0.0.0 --setenv=PORT=80 --setenv=EXTRA_PORT=81)
for kv in "$@"; do envs+=(--setenv="$kv"); done
systemd-run -q --unit=muc-srv -p User=challenge -p AllowedCPUs=$SRV_CPUS -p LimitNOFILE=8388608 \
  -p AmbientCapabilities=CAP_NET_BIND_SERVICE -p WorkingDirectory=$DIR "${envs[@]}" /bin/bash $DIR/start.sh
for i in $(seq 120); do curl -sf -m 1 http://127.0.0.1:80/health >/dev/null && break; sleep 0.5; done
PID=$(systemctl show -p MainPID --value muc-srv)
TOKEN=$(python3 -c 'import json;t=json.load(open("/opt/muc/tokens.json"));print(t[0]["token"])' 2>/dev/null)
taskset -c $OTHER_CPUS python3 - <<'PY'
import socket, struct, time, itertools, random
targets = [(f"127.0.0.{i}", p) for i in range(2, 10) for p in (80, 81)]
cyc = itertools.cycle(targets)
def burst(n, fn, batch=2000):
    done = 0; t0 = time.time()
    while done < n:
        socks = []
        for _ in range(min(batch, n - done)):
            s = socket.socket(); s.setblocking(False)
            try: s.connect_ex(next(cyc))
            except OSError: pass
            socks.append(s)
        time.sleep(0.05)
        for s in socks:
            try: fn(s)
            except OSError: pass
            s.close()
        done += len(socks)
    return time.time() - t0
def rst(s):
    if random.random() < 0.5: s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
print("1 connect+close %.1fs" % burst(30000, rst))
print("2 half request %.1fs" % burst(20000, lambda s: s.send(b"GET /feed HTTP/1.1\r\nHo")))
full = b"GET /feed HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\n\r\n"
def unread(s):
    s.send(full); time.sleep(0)
    if random.random() < 0.5: s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
print("3 unread answers %.1fs" % burst(20000, unread))
pipe = full * 64
def slow(s):
    s.setblocking(True); s.settimeout(2)
    s.send(pipe)
    got = 0
    try:
        for _ in range(4):
            got += len(s.recv(512)); time.sleep(0.01)
    except OSError: pass
t0 = time.time()
socks = []
for i in range(2000):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    try: s.connect(next(cyc)); s.setblocking(False); s.send(pipe); socks.append(s)
    except OSError: s.close()
time.sleep(3)
for s in socks: s.close()
print("4 pipelined slow readers %d conns %.1fs" % (len(socks), time.time() - t0))
PY
sleep 3
NPID=$(systemctl show -p MainPID --value muc-srv)
H=$(curl -s -o /dev/null -w '%{http_code}' -m 5 http://127.0.0.1:81/health)
F=$(curl -s -m 5 -H 'Accept-Encoding: gzip' http://127.0.0.1:80/feed | gunzip -c 2>/dev/null | head -c 1)
P=$(curl -s -o /dev/null -w '%{http_code}' -m 5 -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
    -d '{"body":"overload check"}' http://127.0.0.1:80/posts)
OPEN=$(ls /proc/$NPID/fd 2>/dev/null | wc -l)
echo "pid $PID -> $NPID, health $H, feed starts '$F', post $P, fds $OPEN, rss $(awk '/VmRSS/{print $2}' /proc/$NPID/status) kB"
journalctl -q _SYSTEMD_INVOCATION_ID="$(systemctl show -p InvocationID --value muc-srv)" --no-pager -o cat | grep -v -i -E "^stats" | tail -5
[ "$PID" = "$NPID" ] && [ "$H" = 200 ] && [ "$F" = "{" ] && [ "$P" = 201 ] && echo OVERLOAD-OK || echo OVERLOAD-FAIL
systemctl stop muc-srv
