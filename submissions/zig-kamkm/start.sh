#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
# Match the benchmark unit's high descriptor limit for direct launches too.
hard_nofile="$(ulimit -Hn 2>/dev/null || true)"
if [ -n "$hard_nofile" ] && [ "$hard_nofile" != "unlimited" ]; then
  ulimit -n "$hard_nofile" 2>/dev/null || true
fi
exec ./feed-zig
