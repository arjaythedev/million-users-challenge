#!/usr/bin/env bash
# Foreground server (systemd ExecStart). Env: DATABASE_URL, JWT_SECRET, HOST, PORT, [EXTRA_PORT], [DB_POOL].
set -euo pipefail
if [ -z "${GOMEMLIMIT:-}" ]; then
  # 1M sockets ≈ 10 GB in kernel; 4 GB headroom; remainder for Go app.
  export GOMEMLIMIT="18GiB"
fi
exec "$(cd "$(dirname "$0")" && pwd)/bin/go-epoll-kaikegm"
