#!/usr/bin/env bash
# Runs the pure-PHP FFI epoll HTTP/1.1 keep-alive server in the foreground.
set -euo pipefail
cd "$(dirname "$0")"

export START_TIME="$(date +%s)"
export WORKERS="${WORKERS:-32}"

exec php \
  -d ffi.enable=1 \
  -d opcache.enable=1 \
  -d opcache.enable_cli=1 \
  -d opcache.jit=1255 \
  -d opcache.jit_buffer_size=64M \
  -d memory_limit=1G \
  -d variables_order=EGPCS \
  -d display_errors=0 \
  index.php
