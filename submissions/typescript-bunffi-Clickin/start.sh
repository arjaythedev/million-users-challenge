#!/usr/bin/env bash
# Foreground (systemd ExecStart). Config from env: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT.
# WORKERS (optional) overrides the worker count, which defaults to the CPUs we may run on (nproc reads the
# affinity mask, so systemd's AllowedCPUs or taskset decide it).
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
BUN=/opt/bun/bin/bun; [ -x "$BUN" ] || BUN=bun
export WORKERS="${WORKERS:-$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN)}"
exec "$BUN" "$DIR/dist/server.js"
