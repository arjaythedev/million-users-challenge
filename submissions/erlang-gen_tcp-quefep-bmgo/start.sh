#!/usr/bin/env bash
# Runs the server in the foreground (systemd ExecStart). Configuration comes from
# DATABASE_URL, JWT_SECRET, HOST, PORT and optional EXTRA_PORT.
set -euo pipefail
cd "$(dirname "$0")"

ulimit -n "$(ulimit -Hn)" 2>/dev/null || true

# A crash dump on a multi-gigabyte process only delays the restart; skip it.
export ERL_CRASH_DUMP_SECONDS=0

NPROC="$(nproc 2>/dev/null || echo 1)"

exec erl \
  +P 4000000 \
  +Q 4000000 \
  +A 16 \
  +S "${NPROC}:${NPROC}" \
  +sbwt none \
  +sbwtdcpu none \
  +sbwtdio none \
  -noshell \
  -noinput \
  -pa _build/default/lib/*/ebin \
  -eval 'case application:ensure_all_started(feed) of
             {ok, _} -> receive after infinity -> ok end;
             Error -> io:format("start failed: ~p~n", [Error]), halt(1)
         end.'
