#!/usr/bin/env bash
# Runs the server in the foreground. Configured by DATABASE_URL, JWT_SECRET, HOST, PORT and EXTRA_PORT
# ( and optionally WORKERS , the number of worker processes , 32 by default ).
set -euo pipefail
cd "$(dirname "$0")"
exec ./build/feedapi
