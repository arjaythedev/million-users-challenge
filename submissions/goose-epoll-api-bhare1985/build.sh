#!/usr/bin/env bash
# Builds bin/server: libpq from the PostgreSQL 18.6 source tarball and ISA-L 2.31.1 (igzip), both SHA-256 pinned and linked in
# statically (the same two the c-epoll entry builds), the Goose compiler from a pinned commit, and server.goose with that compiler.
set -euo pipefail
cd "$(dirname "$0")"
PG=postgresql-18.6 PGSHA=983ee554ec53dbeb9b70797bef9fcf4e67e117e7e48ca1463cc80b3ff8e8ff3f
IS=isa-l-2.31.1 ISSHA=e1d5573a4019738243b568ab1e1422e6ab7557c5cae33cc8686944d327ad6bb4
GOOSE_COMMIT=2ed1a63dde2ddff777fbb7f4790412b4f00cd47d
# the benchmark box is x86-64; other machines (local testing) build for their own CPU
if [ "$(uname -m)" = x86_64 ]; then MARCH=-march=x86-64-v2; else MARCH=-mcpu=native; fi
mkdir -p bin deps
if [ ! -f deps/$PG/src/interfaces/libpq/libpq.a ]; then
  curl -fsSL -o deps/$PG.tar.gz https://ftp.postgresql.org/pub/source/v18.6/$PG.tar.gz
  echo "$PGSHA  deps/$PG.tar.gz" | sha256sum -c -
  rm -rf deps/$PG && tar -xzf deps/$PG.tar.gz -C deps
  # plain TCP + SCRAM only: no TLS, GSSAPI, readline, zlib or ICU
  (cd deps/$PG && CFLAGS="-O2 $MARCH" ./configure -q --without-readline --without-zlib --without-icu \
     --without-openssl --without-gssapi --without-ldap >/dev/null \
   && make -s -C src/interfaces/libpq -j"$(nproc)" all-static-lib >/dev/null)
fi
if [ ! -f deps/$IS/bin/isa-l.a ]; then
  curl -fsSL -o deps/$IS.tar.gz https://github.com/intel/isa-l/archive/refs/tags/v2.31.1.tar.gz
  echo "$ISSHA  deps/$IS.tar.gz" | sha256sum -c -
  rm -rf deps/$IS && tar -xzf deps/$IS.tar.gz -C deps
  make -s -C deps/$IS -f Makefile.unx -j"$(nproc)" lib >/dev/null  # its x86-64 assembly needs nasm (install.sh)
fi
# The Goose compiler, from source at a pinned commit (the v2026.3 release predates root structs and slice_pool).
GOOSE="${GOOSE:-}"
if [ -z "$GOOSE" ]; then
  if [ ! -x deps/goose/build/goose ]; then
    [ -d deps/goose/.git ] || git clone -q https://github.com/aardappel/goose.git deps/goose
    git -C deps/goose checkout -q $GOOSE_COMMIT
    cmake -S deps/goose -B deps/goose/build -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build deps/goose/build -j"$(nproc)" >/dev/null
  fi
  GOOSE="$PWD/deps/goose/build/goose"
fi
P=deps/$PG/src
mkdir -p build
"$GOOSE" -O2 -o build/server.c server.goose >build/goose.log 2>&1 || { cat build/goose.log; exit 1; }
"$GOOSE" --emit-runtime build/goose_runtime.c >/dev/null
gcc -O2 $MARCH -c build/goose_runtime.c -o build/goose_runtime.o
gcc -O2 $MARCH -I$P/interfaces/libpq -I$P/include -Ideps/$IS/include -c shim.c -o build/shim.o
gcc -O2 $MARCH -Wall -c io.c -o build/io.o
gcc -O2 $MARCH build/server.c build/goose_runtime.o build/shim.o build/io.o \
  $P/interfaces/libpq/libpq.a $P/common/libpgcommon_shlib.a $P/port/libpgport_shlib.a deps/$IS/bin/isa-l.a \
  -lm -pthread -ldl -o bin/server
