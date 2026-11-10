#!/usr/bin/env bash
# Runs the unit tests plus the tests that need Postgres (the driver, and the server over real sockets), each on its
# own fresh copy of feed_seed (feed_pg_test, feed_server_test), the same way test/run.sh copies the seed. Needs psql and the feed_seed database (bash seed/make-seed.sh).
# Connection: the usual libpq env vars (PGHOST, PGPORT, PGUSER, PGPASSWORD).
# Optional: PG_TEST_SCRAM_URL=postgres://user:pass@127.0.0.1:5432/feed_pg_test for a role that logs in with SCRAM
# (needs a scram-sha-256 line for that role in pg_hba.conf, above any trust line, and read/write grants).
set -euo pipefail
cd "$(dirname "$0")"
PGU="${PGUSER:-$(whoami)}"; AUTH="$PGU${PGPASSWORD:+:$PGPASSWORD}"
H="${PGHOST:-localhost}"; case "$H" in /*) H=localhost;; esac
for DB in feed_pg_test feed_server_test; do
  psql -qX -v ON_ERROR_STOP=1 -d postgres -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" -c "CREATE DATABASE $DB TEMPLATE feed_seed"
done
export PG_TEST_URL="postgres://$AUTH@$H:${PGPORT:-5432}/feed_pg_test"
export PG_TEST_SERVER_URL="postgres://$AUTH@$H:${PGPORT:-5432}/feed_server_test"
./mvnw -B -q test
