#!/usr/bin/env bash
# Runs the server in the foreground. Config: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT (SPEC.md); knobs in README.md.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# Soft open-files limit = hard limit (a no-op under systemd, which sets both; needed from a login shell).
ulimit -n "$(ulimit -Hn)" 2>/dev/null || true
if [ -x /usr/local/dotnet/dotnet ]; then export DOTNET_ROOT=/usr/local/dotnet; fi
export DOTNET_TieredPGO="${DOTNET_TieredPGO:-1}"
exec "$HERE/out/Server"
