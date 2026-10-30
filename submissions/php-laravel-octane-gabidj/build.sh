#!/usr/bin/env bash
# As a normal user. Installs the exact tree from composer.lock (production only) and caches the routes. Idempotent.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p storage/framework/{cache,sessions,views} storage/logs bootstrap/cache
composer install --no-dev --no-interaction --no-progress --prefer-dist --optimize-autoloader --classmap-authoritative
# config is NOT cached: it reads DATABASE_URL at start. Routes are compiled once.
php artisan route:clear >/dev/null
php artisan route:cache
php -r 'require "vendor/autoload.php"; echo "deps ok, php ".PHP_VERSION.", swoole ".phpversion("swoole").PHP_EOL;'
