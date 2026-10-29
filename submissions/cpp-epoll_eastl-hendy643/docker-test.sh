#!/usr/bin/env bash
# Runs inside the `test` container of docker-compose.test.yml: seeds Postgres once, then does exactly what CI does,
# `bash test/run.sh submissions/<this folder>` (build.sh, start.sh, the 42 checks in test/test.sh).
set -euo pipefail
SUB=submissions/cpp-epoll_eastl-hendy643

# Work on a copy so the seed and the build never write into the repository on the host. The build directory is a
# volume, so CPM's downloads and the compiled dependencies survive between runs.
cd /src
tar --exclude="$SUB/build" --exclude='cmake-build-*' --exclude=.idea -cf - schema.sql seed test "$SUB" | tar -C /work -xf -
cd /work
# A checkout may have CRLF line endings, which bash rejects ("set: pipefail\r").
# seed/make-seed.sh splits schema.sql at a marker line, which a trailing \r hides.
find . -path ./"$SUB"/build -prune -o -type f \( -name '*.sh' -o -name '*.mjs' -o -name '*.sql' \) -exec sed -i 's/\r$//' {} +

if ! psql -qXAt -d postgres -c "SELECT 1 FROM pg_database WHERE datname = 'feed_seed'" | grep -q 1; then
  echo "== seeding feed_seed (about a minute, once: it stays in the pgdata volume)"
  bash seed/make-seed.sh
fi

exec bash test/run.sh "$SUB"
