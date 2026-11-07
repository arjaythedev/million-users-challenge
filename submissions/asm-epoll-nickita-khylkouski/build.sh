#!/usr/bin/env bash
# Assembles src/*.s with GNU as and links a static binary with no libc: build/server
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$DIR/src"; OUT="$DIR/build"
mkdir -p "$OUT"
rm -f "$OUT"/*.o
for s in "$SRC"/*.s; do
  as --64 -I "$SRC" -o "$OUT/$(basename "${s%.s}").o" "$s"
done
ld -static -nostdlib -z noexecstack -e _start -o "$OUT/server" "$OUT"/*.o
echo "built $OUT/server ($(stat -c %s "$OUT/server") bytes)"
