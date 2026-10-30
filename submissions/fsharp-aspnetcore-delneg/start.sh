#!/usr/bin/env bash
# Runs the server in the foreground. Config: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT (SPEC.md); DB_POOL optional.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ulimit -n "$(ulimit -Hn)" 2>/dev/null || true   # a no-op under systemd; needed from a login shell
if [ -x /usr/local/dotnet/dotnet ]; then export DOTNET_ROOT=/usr/local/dotnet; fi
# Thread pool workers don't spin waiting for work: -10% CPU per request, same latency (worklog e1).
export DOTNET_ThreadPool_UnfairSemaphoreSpinLimit="${DOTNET_ThreadPool_UnfairSemaphoreSpinLimit:-0}"
# Socket completions run on the epoll threads, and Kestrel runs requests there too (worklog e7).
export DOTNET_SYSTEM_NET_SOCKETS_INLINE_COMPLETIONS="${DOTNET_SYSTEM_NET_SOCKETS_INLINE_COMPLETIONS:-1}"
exec "$HERE/out/Server"
