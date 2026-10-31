#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04. Builds Ruby 4.0.9 with YJIT from the official tarball into
# /opt/ruby-4.0.9 (idempotent: skipped if that Ruby is already there with YJIT).
set -euo pipefail
RUBY_VERSION=4.0.9
RUBY_SHA256=c35350682ff050b79702d57ade4516e7336063f510155d19fb2fb5b3f3909511
PREFIX=/opt/ruby-$RUBY_VERSION

export DEBIAN_FRONTEND=noninteractive
apt-get update
# build-essential/autoconf: Ruby + C extensions. rustc: YJIT. libpq-dev: the pg gem (built from source).
apt-get install -y --no-install-recommends \
  build-essential autoconf pkg-config ca-certificates curl rustc \
  libssl-dev libyaml-dev zlib1g-dev libffi-dev libreadline-dev libgmp-dev \
  libpq-dev

if [ -x "$PREFIX/bin/ruby" ] && "$PREFIX/bin/ruby" --yjit -e 'exit(RubyVM::YJIT.enabled? ? 0 : 1)' 2>/dev/null; then
  echo "ruby $RUBY_VERSION with YJIT already installed at $PREFIX"
else
  TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
  cd "$TMP"
  curl -fsSL -o ruby.tar.gz "https://cache.ruby-lang.org/pub/ruby/${RUBY_VERSION%.*}/ruby-$RUBY_VERSION.tar.gz"
  echo "$RUBY_SHA256  ruby.tar.gz" | sha256sum -c -
  tar xzf ruby.tar.gz
  cd "ruby-$RUBY_VERSION"
  ./configure --prefix="$PREFIX" --enable-yjit --disable-install-doc --enable-shared=no
  make -j"$(nproc)"
  make install
fi

"$PREFIX/bin/ruby" --yjit -e 'abort "YJIT missing" unless RubyVM::YJIT.enabled?; puts "#{RUBY_DESCRIPTION}"'
"$PREFIX/bin/bundle" -v
