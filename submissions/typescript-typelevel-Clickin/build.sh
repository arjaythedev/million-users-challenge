#!/usr/bin/env bash
# Build: install the pinned toolchain deps, then assemble the type program.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
export PATH="/opt/node22/bin:$PATH"
corepack enable >/dev/null 2>&1 || true
if command -v pnpm >/dev/null 2>&1; then pnpm install --frozen-lockfile || pnpm install; else npm ci || npm install; fi
# Assemble generated/: hand-written logic types + machine-generated lookup tables.
rm -rf generated && mkdir -p generated
cp types/*.d.ts types/app.ts generated/
node gen/sha.mjs            # -> generated/sha.d.ts (SHA-256 / HMAC tables)
echo "build ok"
