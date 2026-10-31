#!/usr/bin/env bash
# Builds the server as a normal user. External dep: libdeflate, plus
# liburing when present (enables the URING=1 io_uring backend). The
# PostgreSQL client is wire-protocol native, no libpq needed.
set -euo pipefail
cd "$(dirname "$0")"
URING=""
URLIB=""
if [ -f /usr/include/liburing.h ]; then
  URING="-DHAVE_URING"
  URLIB="-luring"
fi
# -luring must follow the objects: Ubuntu links with --as-needed and
# drops libraries named before the sources that reference them
g++ -O3 -march=native -flto $URING -o feed_server src/*.cpp -ldeflate -lpthread $URLIB
