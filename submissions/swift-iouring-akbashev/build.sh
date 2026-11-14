#!/usr/bin/env bash
# Builds the server -> ./.build/release/Feed. Runs as a normal user; no dependencies to download.
#   -enforce-exclusivity=unchecked  no dynamic exclusivity checks (a reactor's state is touched by its own thread).
#   --static-swift-stdlib           the Swift runtime linked in: direct calls instead of PLT stubs.
#   -target-cpu <this CPU>          code for the box's own CPU; Swift has no "native", so the toolchain's clang says
#                                   which CPU it detects, and nothing is added if it can't tell.
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/opt/swift/usr/bin:$PATH"
FLAGS=(-c release -Xswiftc -enforce-exclusivity=unchecked)
[ "$(uname -s)" = Linux ] && FLAGS+=(--static-swift-stdlib)
CPU=$(clang -march=native -### -c -x c /dev/null 2>&1 | grep -o '"-target-cpu" "[^"]*"' | cut -d'"' -f4 || true)
if [ -n "$CPU" ] && [ "$CPU" != generic ]; then
  FLAGS+=(-Xswiftc -target-cpu -Xswiftc "$CPU")
fi
swift build "${FLAGS[@]}"
echo "built ./.build/release/Feed for ${CPU:-a generic CPU} ($(swift --version 2>&1 | head -1))"
