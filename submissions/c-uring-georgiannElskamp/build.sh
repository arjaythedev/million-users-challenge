#!/usr/bin/env bash
# Builds bin/server from the C sources in src/. No dependencies beyond gcc and the kernel headers.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin
if [ "$(uname -m)" = x86_64 ]; then MARCH="-march=x86-64-v2 -mtune=skylake-avx512"; else MARCH=-mcpu=native; fi
gcc -O2 $MARCH -flto=auto -fno-plt -fno-semantic-interposition -Wall -Wextra -Wno-unused-parameter -Wno-unused-result \
    -Wno-sign-compare src/main.c -o bin/server
