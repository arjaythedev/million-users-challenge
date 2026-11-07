#!/usr/bin/env bash
# Compiles src/*.lisp with SBCL and saves ./server (2 GB heap per worker process).
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
sbcl --dynamic-space-size 2048 --noinform --non-interactive --no-sysinit --no-userinit --load build.lisp
