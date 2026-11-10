#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin
cc -O3 -Wall -Wextra -c platform.c -o bin/platform.o
"${FBC:-/opt/freebasic-1.10.1/bin/fbc}" -O 3 -w all main.bas bin/platform.o -l event -l pq -l jansson -l crypto -x bin/goto-million
