#!/usr/bin/env bash
# Forces a Postgres connection's ring of in-flight statements to fill, and checks that no request is answered 500.
# Builds a copy of this folder with RING = 16 (instead of 1024), runs it on one CPU against a database that DATABASE_URL names
# (a throw-away copy of the seed: it gets new posts). A lock on posts held for 4 s stops the inserts from committing, and 800
# POST /posts fired meanwhile are far more than the 16 connections' rings (14 each) can hold. Every one must still be a 201
# once the lock is released: waiting, not a 500. Needs curl and psql. Usage: DATABASE_URL=... tests/full_ring.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"; : "${DATABASE_URL:?}"
TMP="$(mktemp -d)"; trap 'kill $SP 2>/dev/null || true; rm -rf "$TMP"' EXIT
(cd "$HERE" && tar --exclude=./bin --exclude=./build --exclude=./deps -cf - .) | tar -xf - -C "$TMP"
ln -s "$(readlink -f "$HERE/deps")" "$TMP/deps"
sed -i 's/^let RING = 1024;/let RING = 16;/' "$TMP/server.goose"; grep -q '^let RING = 16;' "$TMP/server.goose"
bash "$TMP/build.sh" >/dev/null
PORT=${PORT:-3900}; export JWT_SECRET=million-users-challenge HOST=127.0.0.1 PORT EXTRA_PORT=$((PORT+1)) API_CONFIG="$TMP/openapi.json"
taskset -c 0 "$TMP/bin/server" 2>"$TMP/log" & SP=$!
for _ in $(seq 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
TOKEN=$(sed -n 's/.*"token": *"\([^"]*\)".*/\1/p' "$HERE/../../seed/tokens.json" | head -1)
psql -qX "$DATABASE_URL" -c "BEGIN; LOCK TABLE posts IN ACCESS EXCLUSIVE MODE; SELECT pg_sleep(4); COMMIT" >/dev/null & LK=$!
sleep 0.7
seq 800 | xargs -P 800 -I{} curl -s -m 30 -o /dev/null -w '%{http_code}\n' -X POST -H "Authorization: Bearer $TOKEN" \
  -d '{"body":"full ring {}"}' "http://127.0.0.1:$PORT/posts" > "$TMP/codes"
wait $LK
sort "$TMP/codes" | uniq -c
bad=$(grep -vc '^201$' "$TMP/codes" || true)
[ "$bad" = 0 ] && echo "PASS: 800 of 800 posts answered 201" || { echo "FAIL: $bad requests were not 201"; exit 1; }
