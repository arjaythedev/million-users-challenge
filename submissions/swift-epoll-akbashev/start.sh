#!/usr/bin/env bash
# Runs the server in the foreground. Config: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT (and the optional
# IO_THREADS and DB_CONNECTIONS).
set -euo pipefail
exec "$(cd "$(dirname "$0")" && pwd)/.build/release/Feed"
