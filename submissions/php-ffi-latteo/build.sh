#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

for f in *.php; do
  [ -e "$f" ] && php -d ffi.enable=1 -l "$f"
done
php -v | head -1
