#!/usr/bin/env bash
# Builds bin/server: libpq from the PostgreSQL 18.6 source tarball and ISA-L 2.31.1 (igzip), both SHA-256 pinned
# and linked in statically.
set -euo pipefail
cd "$(dirname "$0")"
PG=postgresql-18.6 PGSHA=983ee554ec53dbeb9b70797bef9fcf4e67e117e7e48ca1463cc80b3ff8e8ff3f
IS=isa-l-2.31.1 ISSHA=e1d5573a4019738243b568ab1e1422e6ab7557c5cae33cc8686944d327ad6bb4
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
P=deps/$PG/src
gcc -O2 $MARCH -flto=auto -fno-plt -pthread -Wall -Wno-unused-result -I$P/interfaces/libpq -I$P/include -Ideps/$IS/include \
  server.c $P/interfaces/libpq/libpq.a $P/common/libpgcommon_shlib.a $P/port/libpgport_shlib.a deps/$IS/bin/isa-l.a -lm -o bin/server
