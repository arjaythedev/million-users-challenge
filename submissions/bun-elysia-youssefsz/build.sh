#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
test "$(bun --version)" = 1.4.2 || { echo 'Bun 1.4.2 is required' >&2; exit 1; }
bun install --frozen-lockfile
bun run check
bun build src/main.ts --target=bun --outdir=dist --minify
