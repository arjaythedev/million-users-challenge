#!/usr/bin/env bash
# Runs the server in the foreground. Config comes from the environment only
# (DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT - SPEC.md).
set -euo pipefail
cd "$(dirname "$0")"
exec ./feed_server
