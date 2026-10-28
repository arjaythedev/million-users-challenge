#!/usr/bin/env bash
# Builds the server as a normal user. External deps: libpq + libdeflate,
# plus liburing when present (enables the URING=1 io_uring backend).
set -euo pipefail
cd "$(dirname "$0")"
URING=""
if [ -f /usr/include/liburing.h ]; then
  URING="-DHAVE_URING -luring"
fi
# libpq-fe.h lives in a distro-specific dir: /usr/include on Arch,
# /usr/include/postgresql on Debian/Ubuntu (pg_config reports it).
PGINC=""
for d in "$(pg_config --includedir 2>/dev/null || true)" \
         /usr/include/postgresql /usr/local/include/postgresql; do
  if [ -n "$d" ] && [ -f "$d/libpq-fe.h" ]; then PGINC="-I$d"; break; fi
done
g++ -O3 -march=native -flto $URING $PGINC -o feed_server src/*.cpp -lpq -ldeflate -lpthread
