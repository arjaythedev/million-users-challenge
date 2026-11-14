#!/usr/bin/env bash
# Pass 15: vendor libdeflate as a static archive. No system install, no -lz.
# Pass 18: vendor picohttpparser (parse-in-place HTTP/1.x parser) the same
# way: pinned tarball + sha256 verify + extract. It compiles as pico.o via
# the Makefile (no separate archive; -I$(PICO_DIR) in CFLAGS).
#
#   build-vendor.sh fetch   download the pinned tarballs + sha256 verify only
#                           (used by install.sh; safe as root, no compiler)
#   build-vendor.sh         fetch + extract + build vendor/libdeflate-<ver>/
#                           libdeflate.a (used by the Makefile; skips work
#                           that is already done)
set -euo pipefail
cd "$(dirname "$0")"
VER=1.26
SHA=125856d4656e0feab660f94842f835923410c9281fedbcee64598c918da42b5a
TARBALL="vendor/libdeflate-${VER}.tar.gz"
URL="https://github.com/ebiggers/libdeflate/releases/download/v${VER}/libdeflate-${VER}.tar.gz"
DIR="vendor/libdeflate-${VER}"
LIB="${DIR}/libdeflate.a"
# Pass 18: picohttpparser pinned to the h2o master commit below (vendored
# like #10/#23: tarball + sha256, static, no system package).
PICO_SHA=dd573d8fa9264c646a400e96ce9f07dea6f156e4
PICO_TAR_SHA=242e0de712cbaf326a507028101916ab8310b34fd4eef112339c71fcbb02a753
PICO_TARBALL="vendor/picohttpparser-${PICO_SHA}.tar.gz"
PICO_URL="https://codeload.github.com/h2o/picohttpparser/tar.gz/${PICO_SHA}"
PICO_DIR="vendor/picohttpparser-${PICO_SHA}"
MODE="${1:-build}"

mkdir -p vendor
if [ ! -f "$TARBALL" ]; then
  curl -fsSL --retry 3 -o "$TARBALL" "$URL"
fi
echo "$SHA  $TARBALL" | sha256sum -c -
if [ ! -f "$PICO_TARBALL" ]; then
  curl -fsSL --retry 3 -o "$PICO_TARBALL" "$PICO_URL"
fi
echo "$PICO_TAR_SHA  $PICO_TARBALL" | sha256sum -c -
if [ "$MODE" = fetch ]; then
  echo "libdeflate $VER tarball ready: $TARBALL"
  echo "picohttpparser $PICO_SHA tarball ready: $PICO_TARBALL"
  exit 0
fi
if [ ! -f "$LIB" ]; then
  rm -rf "$DIR"
  tar xzf "$TARBALL" -C vendor
  # Upstream 1.26 ships CMake only (no plain Makefile), and we want no new
  # build dependency: compile the static archive directly with gcc+ar.
  # -fPIC so it links cleanly into our (PIE) binary.
  mkdir -p "$DIR/build"
  for src in "$DIR"/lib/*.c "$DIR"/lib/x86/cpu_features.c; do
    gcc -O3 -fPIC -I"$DIR" -c "$src" -o "$DIR/build/$(basename "$src" .c).o"
  done
  ar rcs "$LIB" "$DIR"/build/*.o
  rm -f "$DIR"/build/*.o
fi
echo "libdeflate $VER vendored: $LIB"
if [ ! -f "$PICO_DIR/picohttpparser.c" ] || [ ! -f "$PICO_DIR/picohttpparser.h" ]; then
  rm -rf "$PICO_DIR"
  tar xzf "$PICO_TARBALL" -C vendor
fi
test -f "$PICO_DIR/picohttpparser.c" && test -f "$PICO_DIR/picohttpparser.h"
echo "picohttpparser $PICO_SHA vendored: $PICO_DIR"
