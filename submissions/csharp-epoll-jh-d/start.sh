#!/usr/bin/env bash
# The server, in the foreground. Required: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT. Optional knobs (LOOPS,
# DB_READ_POOL, DB_WRITE_POOL, DB_DEAL, BATCH_US, PG_TIMEOUT_MS, STATS_SEC, GZIP_LEVEL) are listed in README.md.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export DOTNET_ROOT=${DOTNET_ROOT:-/usr/share/dotnet}
exec "$here/out/FeedServer"
