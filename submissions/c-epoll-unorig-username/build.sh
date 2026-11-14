#!/usr/bin/env bash
# Builds bin/server: one C file plus ISA-L 2.31.1 (igzip) and liburing 2.12 from their SHA-256 pinned source
# tarballs, both linked in statically. The Postgres client is in server.c (wire protocol v3), so there is no libpq.
set -euo pipefail
cd "$(dirname "$0")"
IS=isa-l-2.31.1 ISSHA=e1d5573a4019738243b568ab1e1422e6ab7557c5cae33cc8686944d327ad6bb4
UR=liburing-liburing-2.12 URSHA=f1d10cb058c97c953b4c0c446b11e9177e8c8b32a5a88b309f23fdd389e26370
# the benchmark box is x86-64; other machines (local testing) build for their own CPU
if [ "$(uname -m)" = x86_64 ]; then MARCH=-march=x86-64-v2; else MARCH=-mcpu=native; fi
mkdir -p bin deps
if [ ! -f deps/$IS/bin/isa-l.a ]; then
  curl -fsSL -o deps/$IS.tar.gz https://github.com/intel/isa-l/archive/refs/tags/v2.31.1.tar.gz
  echo "$ISSHA  deps/$IS.tar.gz" | sha256sum -c -
  rm -rf deps/$IS && tar -xzf deps/$IS.tar.gz -C deps
  make -s -C deps/$IS -f Makefile.unx -j"$(nproc)" lib >/dev/null  # its x86-64 assembly needs nasm (install.sh)
fi
if [ ! -f deps/$UR/src/liburing.a ]; then
  curl -fsSL -o deps/liburing-2.12.tar.gz https://github.com/axboe/liburing/archive/refs/tags/liburing-2.12.tar.gz
  echo "$URSHA  deps/liburing-2.12.tar.gz" | sha256sum -c -
  rm -rf deps/$UR && tar -xzf deps/liburing-2.12.tar.gz -C deps
  (cd deps/$UR && ./configure >/dev/null && make -s -C src liburing.a >/dev/null)
fi
# -DNO_URING builds the loop with plain read/write only (liburing is then not needed)
gcc -O2 $MARCH -flto=auto -fno-plt -pthread -Wall -Wno-unused-result -DNO_HEDGE -Ideps/$IS/include -Ideps/$UR/src/include \
  server.c deps/$IS/bin/isa-l.a deps/$UR/src/liburing.a -o bin/server
