#!/bin/bash
# Profiles the server under callgrind, to see where a request's time goes. It builds the server ( build.sh ) plus a
# copy with debug info, starts one worker under valgrind on a fresh copy of the seed, sends it REQUESTS requests
# shaped like the benchmark's users ( GET /feed , GET /posts/:id , 15 % likes , 2 % new posts , gzip ) over
# CONNECTIONS keep-alive connections, stops it so callgrind writes its profile, and opens that in kcachegrind.
#
# Two things callgrind cannot do here , said plainly so the profile is read for what it is :
#  - it cannot run the io_uring transport : valgrind holds its lock across the ring thread's blocking
#    io_uring_enter , so nothing is ever answered. The server is therefore started on its epoll fallback ( an
#    LD_PRELOAD shim makes io_uring_setup fail , as on CI ). Planning , JWT , JSON and gzip are the same code
#    on both ; the ring, its batching, its own Postgres wire code and the deferred sends are not ( the
#    fallback answers through libpq ).
#  - it sees user space only. Under load most of the ring thread's time is in the kernel ( io_uring , TCP ) ;
#    for that use perf on the real transport ( it needs kernel.perf_event_paranoid <= 1 ).
#
#   ./profile.sh [requests=20000] [connections=50]
#
# Needs valgrind ( kcachegrind to look at the result ), python3, curl, and a Postgres holding feed_seed
# ( bash seed/make-seed.sh ), reached through the usual libpq variables ( PGHOST, PGPORT, PGUSER, PGPASSWORD )
# exactly as test/run.sh does. The only database it touches is feed_profile, which it drops and recreates.
# Everything it writes goes to build/profile/ .
set -euo pipefail

die() { echo "profile.sh: $*" >&2; exit 1; }

THISDIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$THISDIR"
ROOT="$( cd "$THISDIR/../.." && pwd )"

[ "$(id -u)" -ne 0 ] || die "do not run this as root"
[ -f "$THISDIR/feedapi.c" ] && [ -f "$THISDIR/build.sh" ] || die "$THISDIR is not the c-ammarserver-AmmarkoV submission"
[ -f "$ROOT/seed/tokens.json" ] || die "no $ROOT/seed/tokens.json : run bash seed/make-seed.sh first"

REQUESTS="${1:-20000}"
CONNECTIONS="${2:-50}"
[[ "$REQUESTS" =~ ^[0-9]+$ ]] && [ "$REQUESTS" -gt 0 ] || die "requests must be a positive number"
[[ "$CONNECTIONS" =~ ^[0-9]+$ ]] && [ "$CONNECTIONS" -gt 0 ] && [ "$CONNECTIONS" -le 2000 ] || die "connections must be 1..2000"

for tool in valgrind python3 curl psql; do command -v "$tool" >/dev/null || die "$tool is not installed"; done

PORT="${PORT:-3300}"
[[ "$PORT" =~ ^[0-9]+$ ]] || die "PORT must be a number"
! curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/" || die "something is already listening on port $PORT"
command -v pgrep >/dev/null || die "pgrep is not installed"

OUT="$THISDIR/build/profile"
mkdir -p "$OUT"
rm -f "$OUT"/callgrind.out.* "$OUT"/server.log "$OUT"/load.py "$OUT"/no_io_uring.so

# ---------------------------------------------------------------- build
# build.sh fetches and builds AmmarServer and the benchmark binary ; the profiled copy is the same source and
# the same flags plus -g , so callgrind can point at lines ( LTO stays on : AmmarServer's archives need it ).
bash ./build.sh
AMMARSERVER="$THISDIR/build/AmmarServer"
PQ_CFLAGS="$(pkg-config --cflags libpq 2>/dev/null || echo -I/usr/include/postgresql)"
echo "== feedapi-profile ( -g )"
cc -O3 -DNDEBUG -march=native -mtune=native -flto=auto -g -fno-omit-frame-pointer -Wall \
   $PQ_CFLAGS \
   -I"$AMMARSERVER/src/AmmServerlib" \
   -o "$OUT/feedapi-profile" \
   feedapi.c jwt.c \
   "$AMMARSERVER/src/AmmServerlib/libAmmarServer.a" \
   "$AMMARSERVER/src/Hashmap/libHashmap.a" \
   "$AMMARSERVER/src/InputParser/libInputParser_C.a" \
   -lpq -lisal -lcrypto -lpthread -lrt -lm

# The shim that puts the server on its epoll transport : io_uring_setup fails with ENOMEM , everything else is
# passed through.
cat > "$OUT/no_io_uring.c" <<'C'
#define _GNU_SOURCE
#include <stdarg.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/syscall.h>
long syscall(long number, ...)
{
  va_list ap;
  va_start(ap,number);
  unsigned long a1=va_arg(ap,unsigned long), a2=va_arg(ap,unsigned long), a3=va_arg(ap,unsigned long);
  unsigned long a4=va_arg(ap,unsigned long), a5=va_arg(ap,unsigned long), a6=va_arg(ap,unsigned long);
  va_end(ap);
  if (number==SYS_io_uring_setup) { errno=ENOMEM; return -1; }
  static long (*real)(long,...) = 0;
  if (real==0) { real=(long (*)(long,...)) dlsym(RTLD_NEXT,"syscall"); }
  return real(number,a1,a2,a3,a4,a5,a6);
}
C
cc -O2 -shared -fPIC -o "$OUT/no_io_uring.so" "$OUT/no_io_uring.c" -ldl

# ---------------------------------------------------------------- database
psql -qXAt -d postgres -c "SELECT 1 FROM pg_database WHERE datname = 'feed_seed'" | grep -q 1 \
  || die "no feed_seed database : run bash seed/make-seed.sh"
psql -qX -v ON_ERROR_STOP=1 -d postgres \
     -c "DROP DATABASE IF EXISTS feed_profile WITH (FORCE)" -c "CREATE DATABASE feed_profile TEMPLATE feed_seed" >/dev/null

PGU="${PGUSER:-$(whoami)}"; AUTH="$PGU${PGPASSWORD:+:$PGPASSWORD}"
H="${PGHOST:-localhost}"; case "$H" in /*) H=localhost;; esac
export DATABASE_URL="postgres://$AUTH@$H:${PGPORT:-5432}/feed_profile"
export JWT_SECRET="${JWT_SECRET:-million-users-challenge}" HOST=127.0.0.1 PORT
# One port and one worker , so the whole profile is one process ; two Postgres connections ( one read , one
# write ) keep the epoll fallback's synchronous path to one round trip per request.
export EXTRA_PORT= WORKERS=1 DB_CONNECTIONS=2

# ---------------------------------------------------------------- server under callgrind
# Callgrind follows the fork , so the parent and the worker each write callgrind.out.<pid>.
echo "== starting the server under callgrind ( log: $OUT/server.log )"
LD_PRELOAD="$OUT/no_io_uring.so" \
valgrind --tool=callgrind --dump-instr=yes --collect-jumps=yes \
         --callgrind-out-file="$OUT/callgrind.out.%p" \
         "$OUT/feedapi-profile" > "$OUT/server.log" 2>&1 &
VG=$!
# SIGTERM to the parent stops its worker too , and callgrind writes each profile as its process exits. Under
# valgrind a process stuck in a blocking call may never take the signal , so whatever of this run is still
# there after 60 s is killed ( without a profile , which is then said ) : nothing it started is left behind.
cleanup()
{
  local workers
  workers="$(pgrep -P "$VG" 2>/dev/null || true)"
  kill -TERM "$VG" 2>/dev/null || true
  for _ in $(seq 120); do
    local alive=0
    for p in "$VG" $workers; do kill -0 "$p" 2>/dev/null && alive=1; done
    [ "$alive" -eq 0 ] && break
    sleep 0.5
  done
  for p in $workers "$VG"; do
    if kill -0 "$p" 2>/dev/null; then echo "profile.sh: process $p did not stop , killed ( no profile from it )" >&2; kill -KILL "$p" 2>/dev/null || true; fi
  done
  wait "$VG" 2>/dev/null || true
}
trap cleanup EXIT

for _ in $(seq 240); do
  curl -sf --max-time 2 "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && break
  kill -0 "$VG" 2>/dev/null || { cat "$OUT/server.log"; die "the server exited while starting"; }
  sleep 0.5
done
curl -sf --max-time 2 "http://127.0.0.1:$PORT/health" >/dev/null || { cat "$OUT/server.log"; die "no healthy /health after 120 s"; }

# ---------------------------------------------------------------- load
cat > "$OUT/load.py" <<'PY'
import asyncio, gzip, json, random, sys, time, collections
PORT = int(sys.argv[1])
TOTAL, CONNS = int(sys.argv[2]), int(sys.argv[3])
TOKENS = json.load(open(sys.argv[4]))
status = collections.Counter()
sent = 0

async def response(r):
    head = await asyncio.wait_for(r.readuntil(b'\r\n\r\n'), 120)
    lines = head.split(b'\r\n'); code = int(lines[0].split()[1]); h = {}
    for line in lines[1:]:
        if b':' in line:
            k, v = line.split(b':', 1); h[k.strip().lower()] = v.strip()
    n = int(h.get(b'content-length', b'0'))
    body = await asyncio.wait_for(r.readexactly(n), 120) if n else b''
    if h.get(b'content-encoding') == b'gzip': body = gzip.decompress(body)
    return code, body

async def request(r, w, label, method, path, token=None, body=None):
    global sent
    sent += 1
    head = '%s %s HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\n' % (method, path)
    if token: head += 'Authorization: Bearer %s\r\n' % token
    if body is not None: head += 'Content-Type: application/json\r\nContent-Length: %d\r\n' % len(body)
    elif method == 'POST': head += 'Content-Length: 0\r\n'
    w.write(head.encode() + b'\r\n' + (body or b'')); await w.drain()
    code, data = await response(r)
    status['%s %d' % (label, code)] += 1
    return code, data

async def user(i):
    rng = random.Random(i)
    token = TOKENS[i % len(TOKENS)]['token']
    r, w = await asyncio.open_connection('127.0.0.1', PORT)
    try:
        while sent < TOTAL:
            code, data = await request(r, w, 'GET /feed', 'GET', '/feed')
            posts = json.loads(data)['posts'] if code == 200 else []
            if sent >= TOTAL: break
            pid = rng.choice(posts)['id'] if posts else rng.randint(1, 500000)
            await request(r, w, 'GET /posts/:id', 'GET', '/posts/%d' % pid)
            if sent < TOTAL and rng.random() < 0.15: await request(r, w, 'POST /posts/:id/like', 'POST', '/posts/%d/like' % pid, token)
            if sent < TOTAL and rng.random() < 0.02:
                await request(r, w, 'POST /posts', 'POST', '/posts', token, json.dumps({'body': 'profile %d' % i}).encode())
    finally:
        w.close()

async def main():
    t0 = time.time()
    await asyncio.gather(*[user(i) for i in range(CONNS)])
    took = time.time() - t0
    print('LOAD %d requests in %.1f s ( %.0f req/s under callgrind )' % (sum(status.values()), took, sum(status.values()) / took))
    for k, v in sorted(status.items()): print('   %-28s %d' % (k, v))

asyncio.run(main())
PY
echo "== $REQUESTS requests over $CONNECTIONS connections"
python3 -I "$OUT/load.py" "$PORT" "$REQUESTS" "$CONNECTIONS" "$ROOT/seed/tokens.json"

# ---------------------------------------------------------------- profile
cleanup
trap - EXIT
WORKER_PROFILE="$(ls -S "$OUT"/callgrind.out.* 2>/dev/null | head -1 || true)"
[ -n "$WORKER_PROFILE" ] || die "callgrind wrote no profile ( see $OUT/server.log )"
echo "== profile : $WORKER_PROFILE ( the largest one is the worker's )"
callgrind_annotate "$WORKER_PROFILE" 2>/dev/null | head -40 || true

if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && command -v kcachegrind >/dev/null; then
  kcachegrind "$WORKER_PROFILE"
fi
exit 0
