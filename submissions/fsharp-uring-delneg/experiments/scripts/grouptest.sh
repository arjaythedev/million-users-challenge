#!/usr/bin/env bash
# grouptest.sh <dir> [ENV=VAL ...]: fresh database, server from <dir> on port 3100 (one thread, so writes group), 3 rounds
DIR=$1; shift
sudo -u postgres psql -qX -c "DROP DATABASE IF EXISTS feed_man WITH (FORCE)" -c "CREATE DATABASE feed_man OWNER feed TEMPLATE feed_seed" >/dev/null 2>&1
sudo -u challenge env "$@" THREADS=1 DATABASE_URL=postgres://feed:feed@127.0.0.1:5432/feed_man JWT_SECRET=million-users-challenge HOST=127.0.0.1 PORT=3100 bash -c "cd $DIR && exec bash start.sh" > /tmp/gt.log 2>&1 &
for i in $(seq 60); do curl -sf http://127.0.0.1:3100/health >/dev/null && break; sleep 0.5; done
for r in 1 2 3; do python3 /opt/muc/grouptest.py 3100 /opt/muc/tokens.json feed_man; done
pkill -f "$DIR/out/Server"; sleep 1
grep -i -c exception /tmp/gt.log
sudo -u postgres psql -qX -c "DROP DATABASE IF EXISTS feed_man WITH (FORCE)" >/dev/null 2>&1
