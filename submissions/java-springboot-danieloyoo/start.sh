#!/usr/bin/env bash
set -euo pipefail

# Get the directory where start.sh is located
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

exec java \
  -XX:+UseG1GC \
  -XX:MaxRAMPercentage=70 \
  -XX:InitialRAMPercentage=10 \
  -jar "$DIR/target/feed.jar" \
  --server.address="${HOST:-0.0.0.0}" \
  --server.port="${PORT:-3000}"