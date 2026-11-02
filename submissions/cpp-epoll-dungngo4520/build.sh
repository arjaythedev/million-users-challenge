#!/usr/bin/env bash
# Builds the server as a normal user. External deps: libdeflate (always),
# isa-l when present (faster gzip), liburing when present (enables the
# URING=1 io_uring backend). The PostgreSQL client is wire-protocol
# native, no libpq needed.
set -euo pipefail
cd "$(dirname "$0")"
URING=""
URLIB=""
if [ -f /usr/include/liburing.h ]; then
  URING="-DHAVE_URING"
  URLIB="-luring"
fi
ISAL=""
ISALIB=""
if [ -f /usr/include/isa-l.h ]; then
  ISAL="-DHAVE_ISAL"
  ISALIB="-lisal"
fi
# static-link libdeflate where the .a ships (Ubuntu -dev has it) to take
# the compress call out of the PLT on every gzipped response
DEFL=-ldeflate
if printf 'int main(){return 0;}\n' | g++ -x c - -l:libdeflate.a -o /dev/null 2>/dev/null; then
  DEFL=-l:libdeflate.a
fi
# -luring must follow the objects: Ubuntu links with --as-needed and
# drops libraries named before the sources that reference them
g++ -O3 -march=native -flto -no-pie -fno-plt -fcf-protection=none \
    -fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables \
    -Wl,-O1 $URING $ISAL -o feed_server src/*.cpp \
    $DEFL $ISALIB -lpthread $URLIB
