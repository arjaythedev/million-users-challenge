#!/usr/bin/env bash
set -euo pipefail
ulimit -n "$(ulimit -Hn)" 2>/dev/null || true
exec "$(cd "$(dirname "$0")" && pwd)/target/release/feed"
