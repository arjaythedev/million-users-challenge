#!/usr/bin/env bash
# Builds the C3 server and ISA-L's CPU-dispatched gzip CRC, pinned and linked statically.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin deps
IS=isa-l-2.31.1
ISSHA=e1d5573a4019738243b568ab1e1422e6ab7557c5cae33cc8686944d327ad6bb4
if [ ! -f "deps/$IS/bin/isa-l.a" ]; then
  curl -fsSL -o "deps/$IS.tar.gz" "https://github.com/intel/isa-l/archive/refs/tags/v2.31.1.tar.gz"
  echo "$ISSHA  deps/$IS.tar.gz" | sha256sum -c -
  rm -rf "deps/$IS"
  tar -xzf "deps/$IS.tar.gz" -C deps
  make -s -C "deps/$IS" -f Makefile.unx -j"$(nproc)" lib
fi
C3C=${C3C:-c3c}
# the benchmark box is x86-64; other machines (local testing) build for their own CPU
case "$(uname -m)" in
  x86_64) TARGET="--target linux-x64 --x86cpu=sse4";;
  aarch64) TARGET="--target linux-aarch64";;
  *) TARGET="";;
esac
"$C3C" compile -O3 --single-module=yes $TARGET --safe=no -z "$PWD/deps/$IS/bin/isa-l.a" -o bin/server *.c3
