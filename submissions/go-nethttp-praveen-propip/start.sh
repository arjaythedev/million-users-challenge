#!/usr/bin/env bash
# Foreground server (systemd ExecStart). Env: DATABASE_URL, JWT_SECRET, HOST,
# PORT, [EXTRA_PORT], [DB_POOL], [DB_TIMEOUT_MS].
set -euo pipefail
# Go's soft memory limit (the documented knob for "this process may use about
# this much RAM"). Without it the GC lets the heap grow to 2x the live heap
# (GOGC=100); at ~1M idle keep-alive connections the live heap plus the
# kernel's socket memory must share 31 GB. 75% of RAM leaves the rest for the
# kernel. An explicit GOMEMLIMIT wins.
if [ -z "${GOMEMLIMIT:-}" ]; then
  export GOMEMLIMIT="$(( $(awk '/^MemTotal:/{print $2}' /proc/meminfo) * 75 / 100 ))KiB"
fi
exec "$(cd "$(dirname "$0")" && pwd)/bin/feedapi"
