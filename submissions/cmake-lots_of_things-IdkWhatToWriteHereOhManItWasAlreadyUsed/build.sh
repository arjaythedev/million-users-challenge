#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

CXXFLAGS="-O3 -march=native -mtune=native -DNDEBUG -std=c++20 -fno-plt -fno-semantic-interposition -pthread"
PQ_INC="/usr/include/postgresql"
APPDIR="${APPDIR:-/app}"

g++ $CXXFLAGS listener/server.cpp -o listener/server -levent -lpthread
g++ $CXXFLAGS -I"$PQ_INC" postgres/pool.cpp     -o postgres/pool     -lpq -lpthread
g++ $CXXFLAGS -I"$PQ_INC" postgres/pool_cli.cpp -o postgres/pool_cli

nasm -f elf64 launcher/launcher.asm -o launcher/launcher.o
ld launcher/launcher.o -o launcher/launcher \
   -dynamic-linker /lib64/ld-linux-x86-64.so.2 -lc

mkdir -p "$APPDIR/server" "$APPDIR/launcher" "$APPDIR/postgres" "$APPDIR/listener"
cp -f server/*.cmake    "$APPDIR/server/"
cp -f launcher/launcher "$APPDIR/launcher/"
cp -f postgres/pool     "$APPDIR/postgres/"
cp -f postgres/pool_cli "$APPDIR/postgres/"
cp -f listener/server   "$APPDIR/listener/server"