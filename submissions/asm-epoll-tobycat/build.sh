#!/usr/bin/env bash
# Assembles src/*.s with GNU as and links ./server statically: no libc, no CRT, raw syscalls only.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
objs=()
for m in server rt crypto json gzip pg resolve; do
  as --64 -I src -o "build/$m.o" "src/$m.s"
  objs+=("build/$m.o")
done
ld -static -nostdlib -z noexecstack -e _start -o server "${objs[@]}"
