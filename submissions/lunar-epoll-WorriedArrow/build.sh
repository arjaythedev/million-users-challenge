#!/usr/bin/env bash
# Builds bin/server from src/ with the Lunar compiler install.sh put in /opt/lunar-0.2.0 (LLVM optimizations on).
set -euo pipefail
cd "$(dirname "$0")"
LUNAR=${LUNAR:-/opt/lunar-0.2.0/bin/lunar}
"$LUNAR" build --release
mkdir -p bin
cp target/release/server bin/server
