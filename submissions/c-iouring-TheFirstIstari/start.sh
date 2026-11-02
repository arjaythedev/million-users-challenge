#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
ulimit -n 1048576 2>/dev/null || true
# Pass 10: cap glibc malloc arenas (share-nothing workers + pooled slabs
# already avoid allocator churn; this bounds virtual memory sprawl).
export MALLOC_ARENA_MAX=2
exec ./server
