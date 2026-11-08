#!/usr/bin/env bash
# start.sh: runs the server in the foreground (systemd ExecStart). Configured
# only by the SPEC runtime-contract env vars:
#   DATABASE_URL  libpq conninfo (postgres://feed:...@dbbox:5432/feed)
#   JWT_SECRET    HS256 secret (million-users-challenge on the box)
#   HOST          bind address (0.0.0.0 on the box)
#   PORT          primary port (80)
#   EXTRA_PORT    second port served by the same process (81)
set -euo pipefail
cd "$(dirname "$0")"

# Raise the soft open-files limit to the hard limit. Under systemd this is a
# no-op (LimitNOFILE=2097152 sets both), but from a login shell the soft limit
# is often 1024, which would cap concurrent keep-alive connections far below the
# pool and make accept() fail EMFILE. Best effort; ignore if not permitted.
ulimit -n "$(ulimit -Hn)" 2>/dev/null || true

# WORKERS: one worker process per available core unless overridden. Each worker
# binds both ports with SO_REUSEPORT and holds its own Postgres connection, so
# the kernel load-balances connections across all cores. On the 16-core box this
# is 16; the test harness can set WORKERS=1 for a single process.
if [ -z "${WORKERS:-}" ]; then
  export WORKERS="$(nproc 2>/dev/null || echo 1)"
fi

# gforth loads the layers (server.fs) and calls epoll-main (the event loop).
# `exec` so the server is PID-visible to the supervisor and receives signals.
exec gforth server.fs
