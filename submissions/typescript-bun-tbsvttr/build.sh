#!/usr/bin/env bash
# Bundle server.ts into dist/server.js with Bun's own bundler. Nothing to download: there are no dependencies.
set -euo pipefail
cd "$(dirname "$0")"
BUN=/opt/bun/bin/bun; [ -x "$BUN" ] || BUN=bun   # /opt/bun from install.sh; any bun on PATH for local runs
"$BUN" build server.ts --target=bun --outfile=dist/server.js
