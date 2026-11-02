#!/usr/bin/env bash
# Pass 12: PGO profile workload (called by 'make pgo', not by CI).
# Starts the -fprofile-generate build on a scratch database, drives it with
# a short 5k-user mload, then SIGTERMs the server (the BUILD_PGO_GEN hook in
# server.c dumps .gcda) and drops the scratch DB.
set -euo pipefail
HERE=$(cd $(dirname $0) && pwd)
MLOAD=$HERE/../../bench/mload/mload
TOKENS=$HERE/../../seed/tokens.json
DB=feed_pgo
H=${PGHOST:-127.0.0.1}
P=${PGPORT:-5545}
U=${PGUSER:-postgres}
export PGPASSWORD=${PGPASSWORD:-postgres}
psql -qX -h $H -p $P -U $U -d postgres -c 'DROP DATABASE IF EXISTS feed_pgo WITH (FORCE)' -c 'CREATE DATABASE feed_pgo TEMPLATE feed_seed'
export DATABASE_URL=postgres://$U:$PGPASSWORD@$H:$P/$DB
export JWT_SECRET=${JWT_SECRET:-million-users-challenge} HOST=127.0.0.1 PORT=3000 EXTRA_PORT=3001
ulimit -n 1048576 2>/dev/null || true
export MALLOC_ARENA_MAX=2
cd $HERE
./server > /tmp/c-iouring-pgo-profile.log 2>&1 &
SRV=$!
cleanup() { kill $SRV 2>/dev/null || true; wait $SRV 2>/dev/null || true; }
trap cleanup EXIT
for _ in $(seq 60); do curl -sf http://127.0.0.1:3000/health >/dev/null && break; sleep 1; done
curl -sf http://127.0.0.1:3000/health >/dev/null
curl -sf http://127.0.0.1:3001/health >/dev/null
ulimit -n 1048576 2>/dev/null || true
rc=0
$MLOAD -target 127.0.0.1 -ports 3000,3001 -users 5000 -tokens $TOKENS -ramp 15s -hold 45s -gzip -think-scale 2.5 || rc=$?
trap - EXIT
cleanup
psql -qX -h $H -p $P -U $U -d postgres -c 'DROP DATABASE IF EXISTS feed_pgo WITH (FORCE)'
echo PGO-PROFILE-DONE rc=$rc
exit $rc
