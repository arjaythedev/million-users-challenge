#!/usr/bin/env bash
# Runs the server in the foreground. Configuration: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT (see SPEC.md).
set -euo pipefail
cd "$(dirname "$0")"
exec bin/feed
