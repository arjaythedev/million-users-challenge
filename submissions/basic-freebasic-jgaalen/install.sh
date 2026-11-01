#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends build-essential curl ca-certificates xz-utils libncurses-dev libffi-dev libgpm-dev libpq-dev libevent-dev libjansson-dev libssl-dev
version=1.10.1
prefix=/opt/freebasic-$version
if [ ! -x "$prefix/bin/fbc" ]; then
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT
  cd "$tmp"
  curl -fL "https://github.com/freebasic/fbc/releases/download/$version/FreeBASIC-$version-source-bootstrap.tar.xz" -o source.tar.xz
  echo '2fae2c66b435240d4de11bb534843baa188574508a8a5bdaa5020bf729bf0a3e  source.tar.xz' | sha256sum -c -
  tar xf source.tar.xz
  cd "FreeBASIC-$version-source-bootstrap"
  make -j"$(nproc)" bootstrap-minimal
  mkdir -p "$prefix"
  cp -a bin inc lib "$prefix/"
fi
"$prefix/bin/fbc" -version
