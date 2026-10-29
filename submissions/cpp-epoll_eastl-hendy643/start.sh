#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
ulimit -n "$(ulimit -Hn)"
# clang is the default; FEED_COMPILER=gcc (also given to build.sh) runs the gcc build instead
if [ "${FEED_COMPILER:-clang}" = gcc ]; then exec build/release-gcc/server; fi
exec build/release/server
