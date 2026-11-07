#!/usr/bin/env bash
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# Use the service's configured hard descriptor limit for accepted sockets.
if hard_nofile="$(ulimit -Hn 2>/dev/null)"; then
  ulimit -n "$hard_nofile" 2>/dev/null || true
fi
# A fixed soft heap ceiling, well inside the 32 GB benchmark box. An explicit
# GOMEMLIMIT in the environment takes precedence.
export GOMEMLIMIT="${GOMEMLIMIT:-24GiB}"
# The process stays small next to the box's memory (about 3 GB resident with a
# million connections in development runs under GOMEMLIMIT=4GiB), so collect
# less often: each cycle scans every connection's state, and GOMEMLIMIT still
# bounds the heap.
export GOGC="${GOGC:-400}"
exec "$DIR/bin/feed-go-kamkm"
