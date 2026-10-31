#!/usr/bin/env bash
# Builds the app as a normal user: installs the pinned gems (from source, no precompiled native gems)
# into ./vendor/bundle. Idempotent.
set -euo pipefail
cd "$(dirname "$0")"
export PATH="${RUBY_PREFIX:-/opt/ruby-4.0.9}/bin:$PATH"

bundle config set --local path vendor/bundle
bundle config set --local force_ruby_platform true   # compile pg against the system libpq, no prebuilt binaries
bundle config set --local frozen true                # Gemfile.lock must match: versions are pinned
bundle install --jobs "$(nproc)"

bundle exec ruby -e 'require "iodine"; require "pg"; puts "#{RUBY_DESCRIPTION}"; puts "iodine #{Iodine::VERSION}, pg #{PG::VERSION}, libpq #{PG.library_version}"'
