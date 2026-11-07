#!/usr/bin/env bash
# Run the server in the foreground. Config via SPEC.md env vars.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
export PATH="/opt/node22/bin:$PATH"
exec node "$HERE/src/server.mjs"
