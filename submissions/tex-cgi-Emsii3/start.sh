#!/usr/bin/env bash
# Foreground server. Env: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT, [WORKERS] (default 2 x cores), [WORKER_MAX_REQUESTS] (default 5000), [DB_POOL] (default min(2 x cores, 64)).
set -euo pipefail
cd "$(dirname "$0")"
exec bin/host
