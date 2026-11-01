#!/usr/bin/env bash
set -euo pipefail
# Leave room for kernel socket memory on the benchmark's dedicated Linux server.
if [ -z "${GOMEMLIMIT:-}" ] && [ -r /proc/meminfo ]; then
  export GOMEMLIMIT="$(( $(awk '/^MemTotal:/{print $2}' /proc/meminfo) * 75 / 100 ))KiB"
fi
exec "$(cd "$(dirname "$0")" && pwd)/bin/feed"
