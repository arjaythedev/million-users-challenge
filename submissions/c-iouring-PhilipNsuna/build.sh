#!/usr/bin/env bash
# Release build into bin/feed. No dependencies beyond libc, pthread and liburing.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin
gcc -std=gnu11 -D_GNU_SOURCE -O2 -march=native -flto -pthread -Wall -Wextra -Wno-unused-parameter -o bin/feed src/*.c -luring
echo "built bin/feed ($(gcc --version | head -1))"
