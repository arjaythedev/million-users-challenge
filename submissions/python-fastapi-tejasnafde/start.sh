#!/usr/bin/env bash
# Runs the server in the foreground (systemd ExecStart). Env: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT (optional), DB_POOL (optional).
set -euo pipefail
cd "$(dirname "$0")"
exec .venv/bin/gunicorn --config gunicorn.conf.py "${APP_MODULE:-app.main:app}"
