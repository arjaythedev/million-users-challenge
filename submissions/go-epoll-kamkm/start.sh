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
# The heap stays small next to the box's memory (connection state is a flat
# table the collector does not scan), so collect less often; GOMEMLIMIT still
# bounds the heap.
export GOGC="${GOGC:-400}"
exec "$DIR/bin/feed-go-epoll-kamkm"
