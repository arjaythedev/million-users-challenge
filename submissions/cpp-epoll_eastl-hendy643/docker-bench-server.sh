#!/usr/bin/env bash
# Runs inside the `server` container of docker-compose.bench.yml: prepares the seeded database and the load
# generator's tokens, builds the submission and serves it in the foreground.
set -euo pipefail
SUB=submissions/cpp-epoll_eastl-hendy643

cd /src
tar --exclude="$SUB/build" --exclude='cmake-build-*' --exclude=.idea -cf - schema.sql seed test "$SUB" | tar -C /work -xf -
cd /work
find . -path ./"$SUB"/build -prune -o -type f \( -name '*.sh' -o -name '*.mjs' -o -name '*.sql' \) -exec sed -i 's/\r$//' {} +

if ! psql -qXAt -d postgres -c "SELECT 1 FROM pg_database WHERE datname = 'feed_seed'" | grep -q 1; then
  echo "== seeding feed_seed (about a minute, once)"
  bash seed/make-seed.sh
fi
if [ ! -s /shared/tokens.json ]; then
  echo "== generating seed/tokens.json for the load generator (once)"
  node seed/generate.mjs /tmp/gen > /dev/null
  cp /tmp/gen/tokens.json /shared/tokens.json
  rm -rf /tmp/gen
fi

# Every run starts from a fresh copy of the seed, like the benchmark does.
psql -qX -v ON_ERROR_STOP=1 -d postgres -c "DROP DATABASE IF EXISTS feed_bench WITH (FORCE)" -c "CREATE DATABASE feed_bench TEMPLATE feed_seed"

echo "== build"
bash "$SUB/build.sh"

export DATABASE_URL="postgres://$PGUSER:$PGPASSWORD@$PGHOST:$PGPORT/feed_bench"
export JWT_SECRET=million-users-challenge HOST=0.0.0.0 PORT=3000 EXTRA_PORT=3001
ulimit -n "$(ulimit -Hn)"
echo "== start"
exec bash "$SUB/start.sh"
