#!/usr/bin/env bash
# The challenge's test/run.sh (42 checks) on the box, on PORT (default 3000), then again with PORT/EXTRA_PORT swapped,
# from a clean copy of SRC (default /opt/muc-aspnet/current) in /opt/muc-aspnet/repo/submissions/ (fresh feed_test each time).
set -u
R=/opt/muc-aspnet/repo; S=$R/submissions/fsharp-aspnetcore-delneg
rm -rf "$S"; mkdir -p "$S"
rsync -a --exclude bin --exclude obj --exclude out "${1:-/opt/muc-aspnet/current}/" "$S/"
chown -R challenge: "$R"
cd "$R"
for ports in "3000 3001" "3001 3000"; do
  set -- $ports
  sudo -u challenge -H env PGHOST=127.0.0.1 PGUSER=feed PGPASSWORD=feed PORT=$1 EXTRA_PORT=$2 \
    bash test/run.sh submissions/fsharp-aspnetcore-delneg 2>&1 | grep -E "FAIL|want|got|passed=|error" 
done
# gzip: the test suite sends no Accept-Encoding; check that a gzipped feed and post unzip to the plain bytes
cd "$S" && sudo -u postgres psql -qX -c "DROP DATABASE IF EXISTS feed_gz WITH (FORCE)" -c "CREATE DATABASE feed_gz OWNER feed TEMPLATE feed_seed" >/dev/null
sudo -u challenge env DATABASE_URL=postgres://feed:feed@127.0.0.1:5432/feed_gz JWT_SECRET=million-users-challenge HOST=127.0.0.1 PORT=3100 \
  bash start.sh > /tmp/mua-gz.log 2>&1 &
for i in $(seq 60); do curl -sf http://127.0.0.1:3100/health >/dev/null && break; sleep 1; done
for p in /feed /posts/1 /nope; do
  a=$(curl -s http://127.0.0.1:3100$p | md5sum); b=$(curl -s -H "Accept-Encoding: gzip" http://127.0.0.1:3100$p | gunzip -f | md5sum)
  [ "$a" = "$b" ] && echo "gzip $p ok" || echo "gzip $p FAIL"
done
pkill -u challenge -f "out/Server"; sleep 1
sudo -u postgres psql -qX -c "DROP DATABASE IF EXISTS feed_gz WITH (FORCE)" >/dev/null
