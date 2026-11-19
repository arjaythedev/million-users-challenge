#!/usr/bin/env bash
# Bundle src/ into dist/server.js with Bun's bundler. No dependencies to download.
set -euo pipefail
cd "$(dirname "$0")"
BUN=/opt/bun/bin/bun; [ -x "$BUN" ] || BUN=bun
"$BUN" build src/main.ts --target=bun --minify --outfile=dist/server.js
