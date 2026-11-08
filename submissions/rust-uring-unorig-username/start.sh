#!/usr/bin/env bash
# Runs the server in the foreground. Config: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT.
set -euo pipefail
ulimit -n "$(ulimit -Hn)"  # every connection is a file descriptor (a no-op under systemd's LimitNOFILE)
exec "$(cd "$(dirname "$0")" && pwd)/target/release/feed"
