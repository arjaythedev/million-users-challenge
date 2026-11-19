#!/usr/bin/env bash
# Builds the app as a normal user: fetches Iodine 0.8.3 from its upstream tag, applies iodine-so_reuseport.patch,
# builds and installs that gem into ./vendor/bundle, then installs the other pinned gem (pg, from source, no
# precompiled native gems) with bundler. Idempotent.
set -euo pipefail
cd "$(dirname "$0")"
export PATH="${RUBY_PREFIX:-/opt/ruby-4.0.9}/bin:$PATH"

IODINE_TAG=v0.8.3
IODINE_COMMIT=be45d0fd3a8a000fbb9a30f934c9ce4741ed0d2e   # what the v0.8.3 tag points to
if [ "$(git -C vendor/iodine rev-parse HEAD 2>/dev/null)" != "$IODINE_COMMIT" ] || ! git -C vendor/iodine diff --quiet; then
  rm -rf vendor/iodine
  git clone --quiet --depth 1 --branch "$IODINE_TAG" https://github.com/boazsegev/iodine.git vendor/iodine
  [ "$(git -C vendor/iodine rev-parse HEAD)" = "$IODINE_COMMIT" ] || { echo "iodine $IODINE_TAG is not $IODINE_COMMIT"; exit 1; }
fi
git -C vendor/iodine apply --check ../../iodine-so_reuseport.patch 2>/dev/null \
  && git -C vendor/iodine apply ../../iodine-so_reuseport.patch
grep -q IODINE_REUSEPORT vendor/iodine/ext/iodine/fio-stl.h || { echo "patch not applied"; exit 1; }

# Build the patched gem and install it where bundler will look (vendor/bundle), unless that exact build is there.
GEM_DIR="vendor/bundle/ruby/$(ruby -e 'print RbConfig::CONFIG["ruby_version"]')"
PATCH_SUM="$(sha256sum iodine-so_reuseport.patch | cut -c1-16)"
if [ "$(cat "$GEM_DIR/.iodine-patch" 2>/dev/null)" != "$PATCH_SUM" ]; then
  (cd vendor/iodine && gem build iodine.gemspec --output ../iodine-0.8.3.gem >/dev/null)
  gem install --local --no-document --install-dir "$GEM_DIR" vendor/iodine-0.8.3.gem
  echo "$PATCH_SUM" > "$GEM_DIR/.iodine-patch"
fi

bundle config set --local path vendor/bundle
bundle config set --local force_ruby_platform true   # compile pg against the system libpq, no prebuilt binaries
bundle config set --local frozen true                # Gemfile.lock must match: versions are pinned
bundle install --jobs "$(nproc)"

bundle exec ruby -e 'require "iodine"; require "pg"; puts "#{RUBY_DESCRIPTION}"; puts "iodine #{Iodine::VERSION} (+SO_REUSEPORT patch), pg #{PG::VERSION}, libpq #{PG.library_version}"'
