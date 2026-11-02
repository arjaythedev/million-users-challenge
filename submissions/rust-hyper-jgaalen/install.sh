#!/usr/bin/env bash
# One-time Ubuntu 24.04 x86_64 toolchain installation (root).
set -euo pipefail
VERSION=1.98.1
PREFIX="/opt/challenge-rust-$VERSION"
if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then
  echo "This installer targets Linux x86_64. Locally install Rust $VERSION with your package manager." >&2
  exit 1
fi
apt-get update
apt-get install -y build-essential curl ca-certificates xz-utils
if [ ! -x "$PREFIX/bin/rustc" ]; then
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT
  archive="rust-$VERSION-x86_64-unknown-linux-gnu.tar.xz"
  cd "$tmp"
  curl --fail --location --proto '=https' "https://static.rust-lang.org/dist/$archive" -o "$archive"
  curl --fail --location --proto '=https' "https://static.rust-lang.org/dist/$archive.sha256" -o "$archive.sha256"
  sha256sum -c "$archive.sha256"
  tar -xf "$archive"
  bash "rust-$VERSION-x86_64-unknown-linux-gnu/install.sh" --prefix="$PREFIX" --without=rust-docs
fi
"$PREFIX/bin/rustc" --version
