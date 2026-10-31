#!/usr/bin/env bash
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# Use the service's configured hard descriptor limit for accepted sockets.
if hard_nofile="$(ulimit -Hn 2>/dev/null)"; then
  ulimit -n "$hard_nofile" 2>/dev/null || true
fi
# Keep enough address space for Go's per-connection stacks and the kernel's TCP
# buffers when the service is holding a very large idle connection population.
if [ -z "${GOMEMLIMIT:-}" ]; then
  memory_kib="$(awk '/^MemTotal:/{print $2}' /proc/meminfo)"
  if [ -n "$memory_kib" ]; then
    export GOMEMLIMIT="$((memory_kib * 75 / 100))KiB"
  fi
fi
exec "$DIR/bin/feed-go-kamkm"
