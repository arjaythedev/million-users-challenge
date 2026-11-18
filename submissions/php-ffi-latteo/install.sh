#!/usr/bin/env bash
# Run as root on Ubuntu 24.04. Idempotent: installs PHP 8.3 CLI (includes FFI & pcntl) + PostgreSQL + OPcache.
set -euo pipefail

if command -v php >/dev/null 2>&1 && php -m | grep -q '^pgsql$' && php -m | grep -q '^FFI$'; then
  echo "PHP $(php -r 'echo PHP_VERSION;') with FFI and pgsql already installed"
else
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y --no-install-recommends php8.3-cli php8.3-pgsql php8.3-opcache
fi

php -v | head -1
