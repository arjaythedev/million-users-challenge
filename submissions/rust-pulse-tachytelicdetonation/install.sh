#!/usr/bin/env bash
set -euo pipefail
RUST_VERSION=1.99.0
RUSTUP_VERSION=1.28.2
PREFIX=/opt/pulse-rust
case "$(uname -m)" in
  x86_64) TRIPLE=x86_64-unknown-linux-gnu; SHA=20a06e644b0d9bd2fbdbfd52d42540bdde820ea7df86e92e533c073da0cdd43c ;;
  aarch64|arm64) TRIPLE=aarch64-unknown-linux-gnu; SHA=e3853c5a252fca15252d07cb23a1bdd9377a8c6f3efa01531109281ae47f841c ;;
  *) echo "unsupported architecture $(uname -m)"; exit 1 ;;
esac
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends ca-certificates curl gcc libc6-dev >/dev/null
export RUSTUP_HOME=$PREFIX/rustup CARGO_HOME=$PREFIX/cargo
if ! "$CARGO_HOME/bin/rustc" --version 2>/dev/null | grep -q "^rustc $RUST_VERSION "; then
  tmp="$(mktemp -d)"
  curl -sSfL "https://static.rust-lang.org/rustup/archive/$RUSTUP_VERSION/$TRIPLE/rustup-init" -o "$tmp/rustup-init"
  echo "$SHA  $tmp/rustup-init" | sha256sum -c --quiet -
  chmod +x "$tmp/rustup-init"
  "$tmp/rustup-init" -y -q --no-modify-path --profile minimal --default-toolchain "$RUST_VERSION"
  rm -rf "$tmp"
fi
chmod -R a+rX "$PREFIX"
"$CARGO_HOME/bin/rustc" --version
