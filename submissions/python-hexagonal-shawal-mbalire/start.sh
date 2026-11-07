#!/usr/bin/env bash
# Runs the server in the foreground. Env: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT.
# Optional adapter choice: FEED_SERVER, FEED_APP, FEED_DB, FEED_JSON, FEED_PROFILE (see README.md).
set -euo pipefail
cd "$(dirname "$0")"
exec .venv/bin/python -m feed
