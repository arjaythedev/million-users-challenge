#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

ZIG_BIN="${ZIG:-}"
if [ -z "$ZIG_BIN" ]; then
  ZIG_BIN="$(command -v zig 2>/dev/null || true)"
fi
if [ -z "$ZIG_BIN" ] || [ ! -x "$ZIG_BIN" ]; then
  for candidate in /opt/zig-0.13.0/zig "${HOME:-/tmp}/.local/zig-0.13.0/zig"; do
    if [ -x "$candidate" ]; then ZIG_BIN="$candidate"; break; fi
  done
fi
[ -n "$ZIG_BIN" ] && [ -x "$ZIG_BIN" ] || { echo "zig 0.13.0 is not installed (run install.sh)" >&2; exit 1; }

ISAL_VERSION=2.31.1
ISAL_DIR="deps/isa-l-${ISAL_VERSION}"
if [ ! -f "$ISAL_DIR/bin/isa-l.a" ]; then
  mkdir -p deps
  archive="deps/isa-l-${ISAL_VERSION}.tar.gz"
  curl --fail --location --retry 3 --silent --show-error \
    "https://github.com/intel/isa-l/archive/refs/tags/v${ISAL_VERSION}.tar.gz" -o "$archive"
  echo "e1d5573a4019738243b568ab1e1422e6ab7557c5cae33cc8686944d327ad6bb4  $archive" | sha256sum -c -
  tar -xzf "$archive" -C deps
  make -s -C "$ISAL_DIR" -f Makefile.unx -j"$(getconf _NPROCESSORS_ONLN)" lib
fi

# Use the same pipeline API on clean runners and developer machines. This
# challenge connects over plain TCP; libcrypto is used separately for JWTs.
PG_VERSION=18.6
PG_DIR="deps/postgresql-${PG_VERSION}"
if [ ! -f "$PG_DIR/src/interfaces/libpq/libpq.a" ]; then
  archive="deps/postgresql-${PG_VERSION}.tar.gz"
  curl --fail --location --retry 3 --silent --show-error \
    "https://ftp.postgresql.org/pub/source/v${PG_VERSION}/postgresql-${PG_VERSION}.tar.gz" -o "$archive"
  echo "983ee554ec53dbeb9b70797bef9fcf4e67e117e7e48ca1463cc80b3ff8e8ff3f  $archive" | sha256sum -c -
  tar -xzf "$archive" -C deps
  (cd "$PG_DIR" && ./configure -q --without-readline --without-zlib --without-icu \
    --without-openssl --without-gssapi --without-ldap && \
    make -s -C src/interfaces/libpq -j"$(getconf _NPROCESSORS_ONLN)" all-static-lib)
fi
PG_SRC="$PG_DIR/src"
"$ZIG_BIN" build-exe src/main.zig -O ReleaseFast -fstrip \
  -I"$PG_SRC/interfaces/libpq" -I"$PG_SRC/include" -I"$ISAL_DIR/include" \
  "$PG_SRC/interfaces/libpq/libpq.a" "$PG_SRC/common/libpgcommon_shlib.a" \
  "$PG_SRC/port/libpgport_shlib.a" "$ISAL_DIR/bin/isa-l.a" -lc -lcrypto -lm -femit-bin=feed-zig
