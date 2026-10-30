#!/usr/bin/env bash
# Run as root on Ubuntu 24.04. Idempotent. Installs PHP 8.3 (Ubuntu's own packages), Composer, and builds the
# Swoole extension from the PECL source tarball (Ubuntu 24.04 has no php-swoole package), with its pgsql support
# (needs libpq) so that pdo_pgsql can run inside coroutines.
set -euo pipefail
SWOOLE_VERSION=6.2.4
export DEBIAN_FRONTEND=noninteractive

if ! command -v php >/dev/null || ! command -v composer >/dev/null; then
  apt-get update
  apt-get install -y --no-install-recommends php8.3-cli php8.3-pgsql php8.3-mbstring php8.3-xml php8.3-curl \
    php8.3-zip php8.3-opcache composer unzip ca-certificates curl
fi

if php -r 'exit(phpversion("swoole") === "'$SWOOLE_VERSION'" && defined("SWOOLE_HOOK_PDO_PGSQL") ? 0 : 1);' 2>/dev/null; then
  echo "swoole $SWOOLE_VERSION already installed"
else
  apt-get install -y --no-install-recommends php8.3-dev build-essential libcurl4-openssl-dev libssl-dev \
    zlib1g-dev libbrotli-dev libpcre2-dev libpq-dev
  tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
  curl -fsSL -o "$tmp/swoole.tgz" "https://pecl.php.net/get/swoole-$SWOOLE_VERSION.tgz"
  tar -xzf "$tmp/swoole.tgz" -C "$tmp"
  cd "$tmp/swoole-$SWOOLE_VERSION"
  phpize
  ./configure --enable-openssl --enable-brotli --enable-swoole-curl=no --enable-sockets=no \
    --enable-mysqlnd=no --enable-swoole-pgsql --enable-swoole-sqlite=no
  make -j"$(nproc)"
  make install
  echo "extension=swoole.so" > /etc/php/8.3/mods-available/swoole.ini
  phpenmod -v 8.3 -s cli swoole
fi
php -v | head -1
php --ri swoole | grep -E '^(Version|Built|Swoole => |pgsql|coroutine)' || true
php -m | grep -E '^(swoole|pdo_pgsql|mbstring|openssl)$'
composer --version
