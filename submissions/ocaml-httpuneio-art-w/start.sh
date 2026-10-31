#!/usr/bin/env bash
# Runs the server in the foreground (for systemd ExecStart).
# Config from the environment:
# DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT. One Eio domain per CPU of the affinity mask.
set -euo pipefail
ulimit -n "$(ulimit -Hn)"
export DOMAINS="${DOMAINS:-$(nproc)}"
exec "$(cd "$(dirname "$0")" && pwd)/_build/default/app/main.exe"
