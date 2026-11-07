#!/usr/bin/env bash
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$DIR/bin"

CIDER_BIN="${CIDER_BIN:-/opt/cider/build/cider}"
[[ -x "$CIDER_BIN" ]] || { echo "Cider compiler not found at $CIDER_BIN" >&2; exit 1; }

CIDER_LIB="${CIDER_LIB:-/opt/cider/modules}"
[[ -d "$CIDER_LIB" ]] || { echo "Cider modules not found at $CIDER_LIB" >&2; exit 1; }
export CIDER_LIB

echo "== [Cider] Compiling 100% pure Cider server.cid..."
"$CIDER_BIN" compile -c "$DIR/src/server.cid" -o "$DIR/src/server.o"

echo "== [Link] Linking bin/server with libpq, libdeflate, and OpenSSL..."
gcc -nostartfiles -O2 "$DIR/src/server.o" -o "$DIR/bin/server" -lpq -ldeflate -lcrypto -lm -lc

echo "== Build complete: $DIR/bin/server"
