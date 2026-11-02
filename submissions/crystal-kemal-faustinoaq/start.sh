#!/usr/bin/env bash
# One Kemal process per core, each listening on both ports with SO_REUSEPORT; the kernel
# spreads connections over them. Stays in the foreground until they exit.
cd "$(dirname "$0")"
ulimit -n "$(ulimit -Hn)" # every user is a keep-alive connection
export WORKERS="${WORKERS:-$(nproc)}"
pids=()
for _ in $(seq "$WORKERS"); do bin/server & pids+=($!); done
trap 'kill "${pids[@]}" 2>/dev/null' TERM INT
wait
