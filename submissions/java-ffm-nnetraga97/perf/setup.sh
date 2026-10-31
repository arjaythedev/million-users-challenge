#!/usr/bin/env bash
# One Ubuntu 24.04 x86_64 VPS standing in for the app box, the database box and the load generator (build step 4 in
# DESIGN.md): the app box's kernel settings (box/setup-app-box.sh), Postgres 18 with the database box's config and
# seed (box/setup-db-box.sh, the server connecting over 127.0.0.1), Go and mload, and this server built.
# Run once as root from a copy of the repo:
#
#   bash perf/setup.sh   (from this submission folder)
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
GO_VERSION=1.27.1
export DEBIAN_FRONTEND=noninteractive

apt-get update -qq
apt-get install -y -qq sysstat jq curl postgresql-common ca-certificates >/dev/null
apt-get install -y -qq linux-tools-common "linux-tools-$(uname -r)" >/dev/null 2>&1 || echo "perf not available for $(uname -r); skipping"

bash "$ROOT/box/setup-app-box.sh"

# Postgres shares this box with the load generator and the server: half the database box's shared_buffers (the seed
# is ~320 MB, so 4 GB still holds all of it), and WAL kept small for a 20 GB disk (the database box allows 16 GB).
# Installed first so the override is in place before setup-db-box.sh loads the seed (the package creates conf.d).
/usr/share/postgresql-common/pgdg/apt.postgresql.org.sh -y >/dev/null
apt-get install -y -qq postgresql-18 >/dev/null
printf 'shared_buffers = 4GB\nmax_wal_size = 2GB\nmin_wal_size = 512MB\n' > /etc/postgresql/18/main/conf.d/zz-one-box.conf
APP_BOX=127.0.0.1 FEED_PASSWORD=feed bash "$ROOT/box/setup-db-box.sh"

# setup-db-box.sh loads the seed as postgres and then only hands the database to feed: its tables (and their
# sequences) stay postgres's, so feed can't read them. Give them to feed in the template, then make a fresh copy.
pg() { sudo -u postgres psql -qX -v ON_ERROR_STOP=1 "$@"; }
pg -d postgres -c "ALTER DATABASE feed_seed WITH ALLOW_CONNECTIONS true"
pg -d feed_seed -c "ALTER TABLE users OWNER TO feed" -c "ALTER TABLE posts OWNER TO feed" -c "ALTER TABLE likes OWNER TO feed"
pg -d postgres -c "ALTER DATABASE feed_seed WITH ALLOW_CONNECTIONS false"
bash "$ROOT/box/reset-db.sh"
PGPASSWORD=feed psql -qX -v ON_ERROR_STOP=1 -h 127.0.0.1 -U feed -d feed \
  -c "SELECT count(*) AS posts FROM posts" \
  -c "BEGIN" -c "INSERT INTO posts (user_id, body) VALUES (1, 'setup check') RETURNING id" -c "ROLLBACK"

# The seed build writes the tokens next to its copy of the seed; mload reads them from the repo (gitignored there).
install -m 644 /tmp/seed-build/tokens.json "$ROOT/seed/tokens.json"
jq -e 'type == "array" and length > 0' "$ROOT/seed/tokens.json" >/dev/null

if ! /usr/local/go/bin/go version 2>/dev/null | grep -q "go$GO_VERSION "; then
  rm -rf /usr/local/go
  curl -fsSL "https://go.dev/dl/go$GO_VERSION.linux-amd64.tar.gz" | tar -C /usr/local -xz
fi
(cd "$ROOT/bench/mload" && /usr/local/go/bin/go build -o mload .)

bash "$HERE/install.sh"
(cd "$HERE" && ./build.sh)

pg -d postgres -At -c "SHOW shared_buffers" -c "SHOW max_wal_size" -c "SHOW min_wal_size" | paste -sd' ' | sed 's/^/postgres: shared_buffers, max_wal_size, min_wal_size = /'
df -h / | tail -1
echo "ready: bash $HERE/perf/run.sh <users> [server cores]"
