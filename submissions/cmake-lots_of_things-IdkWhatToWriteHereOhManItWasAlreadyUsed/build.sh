#!/usr/bin/env bash
set -euo pipefail

CXXFLAGS="-O3 -march=native -mtune=native -DNDEBUG -std=c++20 -fno-plt -fno-semantic-interposition -pthread"
LDFLAGS="-levent -lpq -lpthread"

g++ $CXXFLAGS listener/server.cpp -o listener/server -levent -lpthread

g++ $CXXFLAGS postgres/pool.cpp    -o postgres/pool    -lpq -lpthread
g++ $CXXFLAGS postgres/pool_cli.cpp -o postgres/pool_cli

nasm -f elf64 launcher/launcher.asm -o launcher/launcher.o
ld launcher/launcher.o -o launcher/launcher \
   -dynamic-linker /lib64/ld-linux-x86-64.so.2 \
   -lc