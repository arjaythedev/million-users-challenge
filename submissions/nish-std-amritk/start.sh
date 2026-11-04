#!/usr/bin/env bash
# Runs the server in the foreground, configured by DATABASE_URL, JWT_SECRET, HOST, PORT and EXTRA_PORT
# (README.md lists the optional tuning variables).
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
# One descriptor per keep-alive connection: as many as the service allows (a no-op under LimitNOFILE).
ulimit -n "$(ulimit -Hn)"
exec "$DIR/bin/feed"
