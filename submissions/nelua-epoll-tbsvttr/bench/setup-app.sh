#!/usr/bin/env bash
# In the app container, once, as root: tools, a copy of the repo in /repo, the seed database (feed_seed) and its
# tokens, these scripts in /bench, and this submission installed and built.
# Postgres comes from the libpq env vars (PGHOST, PGUSER, PGPASSWORD).
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq postgresql-client nodejs curl ca-certificates procps iproute2 >/dev/null
mkdir -p /repo /bench
cd /src && tar --exclude=.git --exclude=node_modules -cf - . | tar -xf - -C /repo
until pg_isready -q; do sleep 1; done
cd /repo
bash seed/make-seed.sh
cp submissions/nelua-epoll-tbsvttr/bench/*.sh seed/tokens.json /bench/
bash submissions/nelua-epoll-tbsvttr/install.sh
bash submissions/nelua-epoll-tbsvttr/build.sh
echo "app container ready: nproc=$(nproc)"
