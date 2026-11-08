#!/usr/bin/env bash
# Run the server in the foreground. Config via SPEC.md env vars.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
export PATH="/opt/node22/bin:$PATH"
# The type checker child (tsgo) stats "." on startup; the rig may launch us from a
# directory we cannot read (e.g. /root), so run from the project directory.
cd "$HERE"
exec node "$HERE/src/server.mjs"
