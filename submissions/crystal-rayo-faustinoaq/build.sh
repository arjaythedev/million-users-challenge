#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin
shards install --frozen --without-development
crystal build src/server.cr --release --no-debug --mcpu x86-64-v3 -Dgc_none -Dwithout_mt -o bin/server
