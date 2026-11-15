#!/usr/bin/env bash
# Foreground server (systemd ExecStart). Env: DATABASE_URL, JWT_SECRET, HOST,
# PORT, EXTRA_PORT.
set -euo pipefail
exec "$(cd "$(dirname "$0")" && pwd)/bin/feedreactor"
