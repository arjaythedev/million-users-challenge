#!/usr/bin/env bash
# Builds bin/server: one C file plus ISA-L 2.31.1 (igzip) from its SHA-256 pinned source tarball, linked in statically.
# The Postgres client is in server.c (wire protocol v3), so there is no libpq.
set -euo pipefail
cd "$(dirname "$0")"
IS=isa-l-2.31.1 ISSHA=e1d5573a4019738243b568ab1e1422e6ab7557c5cae33cc8686944d327ad6bb4
# the benchmark box is x86-64; other machines (local testing) build for their own CPU
if [ "$(uname -m)" = x86_64 ]; then MARCH=-march=x86-64-v2; else MARCH=-mcpu=native; fi
mkdir -p bin deps
if [ ! -f deps/$IS/bin/isa-l.a ]; then
  if [ ! -f deps/$IS.tar.gz ]; then
    curl -fsSL -o deps/$IS.tar.gz https://github.com/intel/isa-l/archive/refs/tags/v2.31.1.tar.gz
  fi
  echo "$ISSHA  deps/$IS.tar.gz" | sha256sum -c -
  rm -rf deps/$IS && tar -xzf deps/$IS.tar.gz -C deps
  make -s -C deps/$IS -f Makefile.unx -j2 lib >/dev/null  # its x86-64 assembly needs nasm (install.sh)
fi
gcc -O2 $MARCH ${BUILD_CFLAGS:-} -flto=2 -fno-plt -pthread -Wall -Wno-unused-result -DNO_HEDGE -DFOUR_CORE_WORKERS=2 -Ideps/$IS/include server.c deps/$IS/bin/isa-l.a -o "${BUILD_OUTPUT:-bin/server}"
