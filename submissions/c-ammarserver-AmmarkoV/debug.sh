#!/usr/bin/env bash
# Builds the rootless local test environment under /tmp/muc-setup ( or $MUC ) : nothing is installed
# system-wide and nothing here is used by install.sh / build.sh / start.sh or the benchmark box.
#
#   bash submissions/c-ammarserver-AmmarkoV/debug.sh        ( from any directory )
#
#   $MUC/pg18        PostgreSQL 18.6 built from the source tarball ( SHA-256 checked )
#   $MUC/pgdata      a cluster on 127.0.0.1:5433 , started , with feed_seed loaded by seed/make-seed.sh
#   $MUC/isal-root   ISA-L 2.31 headers and libraries , unpacked from Ubuntu's libisal2 / libisal-dev .debs
#   $MUC/go , mload  Go 1.27.1 ( checksum from go.dev ) and the load generator built with it
#   $MUC/failuring.so  LD_PRELOAD shim that makes io_uring_setup fail , to exercise the epoll fallback
#   $MUC/run_case2.sh , run_high.sh  pinned load runs ( server 0-3 , Postgres 4-11 , mload 12-15 )
#
# Every step is skipped when its output already exists, so rerunning it only fills in what is missing.
# Needs gcc, make, curl, python3, node >= 18 and apt-get ( for `apt-get download` , which needs no root ).
#
# Safety : it works only inside $MUC , which must be an absolute path of its own ( not / , not $HOME , not
# inside this repository ) and either new , empty , or already one of these environments. It only ever drops
# databases on a Postgres it has confirmed is the one in $MUC/pgdata , and the load scripts it writes only
# stop processes they started themselves.
set -euo pipefail

die() { echo "debug.sh: $*" >&2; exit 1; }

[ "$(id -u)" != 0 ] || die "run this as your normal user, not root ( initdb refuses root, and nothing here needs it )"

# ------------------------------------------------------------------ where things are
# The repository is found from this script's own location, never from the current directory, and checked.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd -P)"
for f in seed/make-seed.sh bench/mload/go.mod test/run.sh schema.sql submissions/c-ammarserver-AmmarkoV/feedapi.c; do
  [ -f "$ROOT/$f" ] || die "$ROOT does not look like the million-users-challenge repository ( no $f )"
done

MUC="${MUC:-/tmp/muc-setup}"
MUC="${MUC%/}"
# Plain characters only : the path is substituted into generated scripts and must not need quoting.
[[ "$MUC" =~ ^/[A-Za-z0-9._/-]+$ ]] || die "MUC must be an absolute path of letters, digits, . _ - and / ( got '$MUC' )"
[[ "$ROOT" =~ ^/[A-Za-z0-9._/-]+$ ]] || die "the repository path '$ROOT' has characters this script does not handle"
case "$MUC" in
  /|/tmp|/home|"$HOME"|/usr|/usr/*|/etc|/etc/*|/opt|/var|/bin|/sbin|/lib|/lib64|/boot|/root)
    die "MUC=$MUC is not a directory of its own ; use something like /tmp/muc-setup" ;;
esac
[ "$(dirname "$MUC")" != /home ] || die "MUC=$MUC is a home directory ; use something like /tmp/muc-setup"
[[ "$MUC" != *"/../"* && "$MUC" != *"/.." && "$MUC" != *"/./"* ]] || die "MUC must not contain . or .. components"
case "$MUC/" in "$ROOT"/*) die "MUC must not be inside the repository ( $ROOT )" ;; esac

# A directory that already exists must be empty or already an environment like this one.
MARKER="$MUC/.muc-setup"
if [ -d "$MUC" ] && [ ! -f "$MARKER" ]; then
  if [ -n "$(ls -A "$MUC")" ] && [ ! -x "$MUC/pg18/bin/postgres" ] && [ ! -d "$MUC/downloads" ]; then
    die "$MUC exists, is not empty and is not a debug environment ; pick another MUC"
  fi
fi
mkdir -p "$MUC/downloads" "$MUC/debs"
touch "$MARKER"

J="$(nproc)"
PG_VERSION=18.6
PG_SHA256=983ee554ec53dbeb9b70797bef9fcf4e67e117e7e48ca1463cc80b3ff8e8ff3f
GO_VERSION=1.27.1
ISAL_DEB_VERSION=2.31.0-0.1build1
PG_PORT=5433
PG_USER="$(id -un)"

# ------------------------------------------------------------------ PostgreSQL 18.6
if [ ! -x "$MUC/pg18/bin/postgres" ]; then
  echo "== PostgreSQL $PG_VERSION"
  TARBALL="$MUC/downloads/postgresql-$PG_VERSION.tar.gz"
  [ -f "$TARBALL" ] || curl -fsSL -o "$TARBALL" "https://ftp.postgresql.org/pub/source/v$PG_VERSION/postgresql-$PG_VERSION.tar.gz"
  echo "$PG_SHA256  $TARBALL" | sha256sum -c -
  rm -rf -- "$MUC/postgresql-$PG_VERSION"     # MUC was validated above ; this is only ever the unpacked source tree
  tar -C "$MUC" -xzf "$TARBALL"
  ( cd "$MUC/postgresql-$PG_VERSION" \
    && ./configure --prefix="$MUC/pg18" --without-readline --without-zlib --without-icu --without-openssl \
                   --without-gssapi --without-ldap > "$MUC/pg-configure.log" \
    && make -j"$J" > "$MUC/pg-make.log" \
    && make install > "$MUC/pg-install.log" )
fi

# ------------------------------------------------------------------ ISA-L, from the Ubuntu packages
if [ ! -f "$MUC/isal-root/usr/include/isa-l/igzip_lib.h" ]; then
  echo "== ISA-L $ISAL_DEB_VERSION"
  # apt-get download checks each .deb against the signed package index, the same as an install would.
  ( cd "$MUC/debs" && apt-get download "libisal2=$ISAL_DEB_VERSION" "libisal-dev=$ISAL_DEB_VERSION" )
  mkdir -p "$MUC/isal-root"
  for deb in "$MUC"/debs/libisal*_"$ISAL_DEB_VERSION"_amd64.deb; do dpkg-deb -x "$deb" "$MUC/isal-root"; done
fi

# ------------------------------------------------------------------ Go 1.27.1 and mload
if [ ! -x "$MUC/go/bin/go" ]; then
  echo "== Go $GO_VERSION"
  TARBALL="$MUC/downloads/go.tar.gz"
  GO_SHA256="$(curl -fsSL 'https://go.dev/dl/?mode=json&include=all' | python3 -I -c '
import json, sys
want = "go" + sys.argv[1] + ".linux-amd64.tar.gz"
for release in json.load(sys.stdin):
    for f in release["files"]:
        if f["filename"] == want: print(f["sha256"])' "$GO_VERSION")"
  [ -n "$GO_SHA256" ] || die "no checksum for go$GO_VERSION on go.dev"
  [ -f "$TARBALL" ] || curl -fsSL -o "$TARBALL" "https://go.dev/dl/go$GO_VERSION.linux-amd64.tar.gz"
  echo "$GO_SHA256  $TARBALL" | sha256sum -c -
  tar -C "$MUC" -xzf "$TARBALL"
fi
if [ ! -x "$MUC/mload" ]; then
  echo "== mload"
  ( cd "$ROOT/bench/mload" && GOCACHE="$MUC/gocache" "$MUC/go/bin/go" build -o "$MUC/mload" . )
fi

# ------------------------------------------------------------------ the io_uring failure shim
if [ ! -f "$MUC/failuring.so" ]; then
  echo "== failuring.so"
  cat > "$MUC/failuring.c" <<'C'
#define _GNU_SOURCE
#include <stdarg.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/syscall.h>

long syscall(long number, ...)
{
  va_list ap;
  unsigned long a1=0,a2=0,a3=0,a4=0,a5=0,a6=0;
  va_start(ap,number);
  a1=va_arg(ap,unsigned long); a2=va_arg(ap,unsigned long); a3=va_arg(ap,unsigned long);
  a4=va_arg(ap,unsigned long); a5=va_arg(ap,unsigned long); a6=va_arg(ap,unsigned long);
  va_end(ap);
  if (number==SYS_io_uring_setup) { errno=ENOMEM; return -1; }
  static long (*real)(long,...) = 0;
  if (real==0) { real=(long (*)(long,...)) dlsym(RTLD_NEXT,"syscall"); }
  return real(number,a1,a2,a3,a4,a5,a6);
}
C
  gcc -O2 -shared -fPIC -o "$MUC/failuring.so" "$MUC/failuring.c" -ldl
fi

# ------------------------------------------------------------------ the load-run scripts
# One template, two shapes : run_case2.sh ( one loopback IP , 30 s ramp , think-scale 0.1 ) and run_high.sh
# ( eight loopback IPs for high connection counts , 60 s ramp , think-scale 0.5 , server on 0.0.0.0 ).
# Rewritten on every run, so they always carry the current safety checks.
write_run_script() { # file ramp host targets think
  echo "== $(basename "$1")"
  sed -e "s#@MUC@#$MUC#g" -e "s#@ROOT@#$ROOT#g" -e "s#@PGPORT@#$PG_PORT#g" -e "s#@PGUSER@#$PG_USER#g" \
      -e "s#@RAMP@#$2#g" -e "s#@HOST@#$3#g" -e "s#@TARGETS@#$4#g" -e "s#@THINK@#$5#g" > "$1.tmp" <<'RUN'
#!/bin/bash
# Usage: run_*.sh <label> <feedapi binary> <workers|auto> <db threads|auto> <users> [hold seconds]
# Generated by submissions/c-ammarserver-AmmarkoV/debug.sh ; safe from any directory.
set -euo pipefail
die() { echo "$(basename "$0"): $*" >&2; exit 1; }
LABEL="${1:?label}"; BIN="${2:?binary}"; W="${3:?workers}"; D="${4:?dbthreads}"; USERS="${5:?users}"; HOLD="${6:-60}"
RAMP=@RAMP@
ROOT=@ROOT@

# The label names files in @MUC@ ; the binary must be a feedapi build , because its process name is what the
# CPU accounting below counts and what a leftover is recognised by.
[[ "$LABEL" =~ ^[A-Za-z0-9._-]+$ ]] || die "label must be letters, digits, . _ - only"
BIN="$(readlink -f -- "$BIN")"
[ -f "$BIN" ] && [ -x "$BIN" ] || die "$BIN is not an executable file"
COMM="$(basename "$BIN" | cut -c1-15)"
case "$COMM" in feedapi*) ;; *) die "the binary's name must start with feedapi ( got $COMM )" ;; esac
[[ "$USERS" =~ ^[0-9]+$ && "$HOLD" =~ ^[0-9]+$ ]] || die "users and hold must be whole numbers"
[ "$W" = auto ] || [[ "$W" =~ ^[0-9]+$ && "$D" =~ ^[0-9]+$ ]] || die "workers / db threads must be numbers or auto"

export LD_LIBRARY_PATH=@MUC@/pg18/lib:@MUC@/isal-root/usr/lib/x86_64-linux-gnu
export PATH=@MUC@/pg18/bin:$PATH
export PGHOST=127.0.0.1 PGPORT=@PGPORT@ PGUSER=@PGUSER@
ulimit -n 1048576

# feed_test is dropped below : only on the debug cluster , never on another Postgres that answers this port.
[ "$(psql -qXAt -d postgres -c 'SHOW data_directory' 2>/dev/null)" = "@MUC@/pgdata" ] \
  || die "127.0.0.1:@PGPORT@ is not the debug cluster in @MUC@/pgdata ( start it with debug.sh )"

if pgrep -u "$(id -u)" -x "$COMM" >/dev/null || pgrep -u "$(id -u)" -x mload >/dev/null; then
  echo "stale benchmark processes present ( stop them first ):" >&2
  pgrep -a -u "$(id -u)" -x "$COMM" >&2 || true; pgrep -a -u "$(id -u)" -x mload >&2 || true; exit 1
fi

psql -qX -d postgres -c "DROP DATABASE IF EXISTS feed_test WITH (FORCE)" -c "CREATE DATABASE feed_test TEMPLATE feed_seed" >/dev/null

export DATABASE_URL=postgres://@PGUSER@@127.0.0.1:@PGPORT@/feed_test
export JWT_SECRET=million-users-challenge HOST=@HOST@ PORT=3100 EXTRA_PORT=3101
EXPECT=0
if [ "$W" = auto ]; then
  unset WORKERS DB_THREADS
else
  export WORKERS="$W" DB_THREADS="$D"; EXPECT=$((W+1))
fi
taskset -c 0-3 "$BIN" >"@MUC@/server2-$LABEL.log" 2>&1 &
SRV=$!
MP=""
# Stops only what this run started : mload by its pid , the server by its pid ( its workers follow it through
# PR_SET_PDEATHSIG ) , and then any worker of that same server that is still around.
cleanup() {
  [ -z "$MP" ] || kill "$MP" 2>/dev/null || true
  WORKER_PIDS="$(pgrep -P "$SRV" 2>/dev/null || true)"
  kill "$SRV" 2>/dev/null || true; wait "$SRV" 2>/dev/null || true
  for i in $(seq 50); do
    alive=""; for p in $WORKER_PIDS; do kill -0 "$p" 2>/dev/null && alive=1; done
    [ -n "$alive" ] || break; sleep 0.1
  done
  for p in $WORKER_PIDS; do kill "$p" 2>/dev/null || true; done
}
trap cleanup EXIT
for i in $(seq 300); do curl -sf http://127.0.0.1:3100/health >/dev/null 2>&1 && curl -sf http://127.0.0.1:3101/health >/dev/null 2>&1 && break; sleep 0.1; done
NP=$(pgrep -u "$(id -u)" -x "$COMM" | wc -l)
if [ "$EXPECT" -ne 0 ] && [ "$NP" -ne "$EXPECT" ]; then die "expected $EXPECT server processes, found $NP"; fi
grep -m1 'workers on' "@MUC@/server2-$LABEL.log" || true

snap() {
python3 -I - <<'PY'
import os
ticks=0; ctxt=0; pgticks=0; n=0; pgn=0
for d in os.listdir('/proc'):
    if not d.isdigit(): continue
    try: comm=open('/proc/%s/comm'%d).read().strip()
    except OSError: continue
    try:
        stat=open('/proc/%s/stat'%d).read(); rest=stat[stat.rfind(') ')+2:].split()
        t=int(rest[11])+int(rest[12])
    except (OSError,IndexError,ValueError): continue
    if comm.startswith('feedapi'): ticks+=t; n+=1
    elif comm=='postgres': pgticks+=t; pgn+=1
    if comm.startswith('feedapi'):
        try:
            for tid in os.listdir('/proc/%s/task'%d):
                try:
                    for line in open('/proc/%s/task/%s/status'%(d,tid)):
                        if line.startswith('voluntary_ctxt_switches:') or line.startswith('nonvoluntary_ctxt_switches:'):
                            ctxt+=int(line.split()[1])
                except OSError: pass
        except OSError: pass
print(ticks,ctxt,pgticks,n,pgn)
PY
}

taskset -c 12-15 env GOMAXPROCS=4 @MUC@/mload -target @TARGETS@ -ports 3100,3101 -users "$USERS" \
  -tokens "$ROOT/seed/tokens.json" -ramp "${RAMP}s" -hold "${HOLD}s" -gzip -think-scale @THINK@ -timeout 10s \
  -out "@MUC@/result2-$LABEL.json" >"@MUC@/mload2-$LABEL.log" 2>&1 &
MP=$!
sleep $((RAMP+25)); MID1=$(snap)
sleep 30; MID2=$(snap)
wait "$MP"; MP=""
python3 -I - "$LABEL" "$W" "$D" "$MID1" "$MID2" "$RAMP" <<'PY'
import json,sys,os
label,w,d,mid1s,mid2s,ramp=sys.argv[1],sys.argv[2],sys.argv[3],sys.argv[4],sys.argv[5],int(sys.argv[6])
mid1=list(map(int,mid1s.split())); mid2=list(map(int,mid2s.split()))
r=json.load(open('@MUC@/result2-%s.json'%label))
tl=[x for x in r.get('timeline',[]) if x['t']>=ramp+25]
span=sum(1 for x in tl)*5
reqs=sum(x['reqs'] for x in tl); fails=sum(x['fails'] for x in tl)
req_s=reqs/span if span else 0
p95=max((x['p95_ms'] for x in tl),default=0); p99=max((x['p99_ms'] for x in tl),default=0)
hz=os.sysconf('SC_CLK_TCK')
cores=(mid2[0]-mid1[0])/hz/30; pgcores=(mid2[2]-mid1[2])/hz/30
us=cores*1e6/req_s if req_s else 0
ctxt=(mid2[1]-mid1[1])/(req_s*30) if req_s else 0
errpct=100*fails/reqs if reqs else 0
print("CASE %-12s users=%6d W=%2s D=%2s steady req/s=%8.0f p95max=%7.1f p99max=%7.1f err=%.3f%% reqs=%d cpu=%.2f cores cpu/req=%.1f us ctx/req=%.3f pg=%.2f cores procs=%d" % (
 label,r['users'],w,d,req_s,p95,p99,errpct,reqs,cores,us,ctxt,pgcores,mid1[3]))
print("WHOLE RUN  req/s=%.0f err=%.3f%% p95=%.1f p99=%.1f errors=%s" % (r['req_per_s'],r['err_pct'],r['p95_ms'],r['p99_ms'],r['errors']))
PY
RUN
  chmod +x "$1.tmp"
  mv -f -- "$1.tmp" "$1"
}
write_run_script "$MUC/run_case2.sh" 30 127.0.0.1 127.0.0.1 0.1
write_run_script "$MUC/run_high.sh"  60 0.0.0.0 \
  127.0.0.1,127.0.0.2,127.0.0.3,127.0.0.4,127.0.0.5,127.0.0.6,127.0.0.7,127.0.0.8 0.5

# ------------------------------------------------------------------ the cluster and the seed
export PATH="$MUC/pg18/bin:$PATH" LD_LIBRARY_PATH="$MUC/pg18/lib"
export PGHOST=127.0.0.1 PGPORT="$PG_PORT" PGUSER="$PG_USER"
if [ ! -f "$MUC/pgdata/PG_VERSION" ]; then
  echo "== initdb"
  initdb -D "$MUC/pgdata" -A trust --locale=C -E UTF8 > "$MUC/initdb.log"
fi
if ! pg_ctl -D "$MUC/pgdata" status >/dev/null 2>&1; then
  echo "== starting PostgreSQL on 127.0.0.1:$PG_PORT ( CPUs 4-11 )"
  taskset -c 4-11 pg_ctl -D "$MUC/pgdata" -l "$MUC/pglog" -w \
    -o "-p $PG_PORT -k /tmp -c listen_addresses=127.0.0.1 -c max_connections=800 -c shared_buffers=2GB \
        -c fsync=off -c synchronous_commit=off -c full_page_writes=off" start > /dev/null \
    || die "PostgreSQL did not start ( see $MUC/pglog ; is something else on port $PG_PORT ? )"
fi
# make-seed.sh drops and recreates feed_seed , so it runs only against the cluster in $MUC/pgdata.
[ "$(psql -qXAt -d postgres -c 'SHOW data_directory' 2>/dev/null)" = "$MUC/pgdata" ] \
  || die "127.0.0.1:$PG_PORT is not the cluster in $MUC/pgdata ; another Postgres is on that port"
if [ "$(psql -qXAt -d postgres -c "SELECT 1 FROM pg_database WHERE datname = 'feed_seed'")" != 1 ] \
   || [ ! -f "$ROOT/seed/tokens.json" ]; then
  echo "== feed_seed ( seed/make-seed.sh )"
  bash "$ROOT/seed/make-seed.sh"
fi

cat <<ENV

Ready. In a shell that builds and tests against it ( every path below is absolute ):

  cd $ROOT
  export PATH=$MUC/pg18/bin:\$PATH
  export CPATH=$MUC/pg18/include:$MUC/isal-root/usr/include
  export LIBRARY_PATH=$MUC/pg18/lib:$MUC/isal-root/usr/lib/x86_64-linux-gnu
  export LD_LIBRARY_PATH=$MUC/pg18/lib:$MUC/isal-root/usr/lib/x86_64-linux-gnu
  export PGHOST=127.0.0.1 PGPORT=$PG_PORT PGUSER=$PG_USER

  PORT=3200 EXTRA_PORT=3201 timeout 400 bash $ROOT/test/run.sh $ROOT/submissions/c-ammarserver-AmmarkoV
  LD_PRELOAD=$MUC/failuring.so PORT=3200 EXTRA_PORT=3201 WORKERS=4 DB_THREADS=2 \\
    timeout 400 bash $ROOT/test/run.sh $ROOT/submissions/c-ammarserver-AmmarkoV      # the epoll fallback
  cp $ROOT/submissions/c-ammarserver-AmmarkoV/build/feedapi $MUC/feedapi-test
  $MUC/run_case2.sh cap50 $MUC/feedapi-test auto auto 50000 60
  $MUC/run_high.sh high200 $MUC/feedapi-test auto auto 200000 60

Stop the cluster with: $MUC/pg18/bin/pg_ctl -D $MUC/pgdata stop -m fast
ENV
