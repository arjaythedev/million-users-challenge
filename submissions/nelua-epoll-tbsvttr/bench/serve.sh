#!/usr/bin/env bash
# In the app container. serve.sh FOLDER [VAR=value ...]: stop the server serve.sh started last, then start FOLDER
# (relative to /repo, e.g. submissions/nelua-epoll-tbsvttr) with its start.sh on a fresh copy of the seed, wait for
# /health on both ports and start the sampler (/tmp/samples.log). serve.sh stop: only stop it.
# Postgres comes from the libpq env vars (PGHOST, PGUSER, PGPASSWORD).
set -euo pipefail
folder=${1:?usage: serve.sh FOLDER [VAR=value ...] | serve.sh stop}; shift
if [ -f /tmp/server.sid ]; then pkill -s "$(cat /tmp/server.sid)" 2>/dev/null || true; rm -f /tmp/server.sid; fi
if [ -f /tmp/sampler.sid ]; then pkill -s "$(cat /tmp/sampler.sid)" 2>/dev/null || true; rm -f /tmp/sampler.sid; fi
[ "$folder" = stop ] && exit 0
for kv in "$@"; do export "$kv"; done
sleep 2
psql -qX -d postgres -c "DROP DATABASE IF EXISTS feed WITH (FORCE)" -c "CREATE DATABASE feed TEMPLATE feed_seed"
export DATABASE_URL="postgres://$PGUSER:$PGPASSWORD@$PGHOST:5432/feed" JWT_SECRET=million-users-challenge
export HOST=0.0.0.0 PORT=3000 EXTRA_PORT=3001
# Its own session, so the next serve.sh stops every process of it (workers, threads) and nothing else.
setsid nohup bash -c 'echo $$ > /tmp/server.sid; exec bash "$0"' "/repo/$folder/start.sh" > /tmp/server.log 2>&1 < /dev/null &
for _ in $(seq 60); do
  curl -sf localhost:3000/health >/dev/null && curl -sf localhost:3001/health >/dev/null && break
  sleep 1
done
curl -s localhost:3000/health; echo
head -3 /tmp/server.log
setsid nohup bash -c 'echo $$ > /tmp/sampler.sid; exec bash /bench/sample.sh "$(cat /tmp/server.sid)"' > /tmp/samples.log 2>&1 < /dev/null &
echo "started $folder (nproc=$(nproc)), sampler running"
