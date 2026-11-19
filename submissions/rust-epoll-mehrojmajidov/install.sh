#!/usr/bin/env bash
# Ubuntu 24.04 x86_64, run once as root. Idempotent.
# Installs a C linker (apt) and Rust 1.99.1 into /opt/feed-rust from the official archives, each checked
# against the SHA-256 pinned below. The crate itself has no dependencies.
set -euo pipefail
VERSION=1.99.1
TARGET=x86_64-unknown-linux-gnu
PREFIX=/opt/feed-rust

if [ -x "$PREFIX/bin/cargo" ] && "$PREFIX/bin/rustc" --version 2>/dev/null | grep -q "^rustc $VERSION "; then
  echo "Rust $VERSION is already in $PREFIX"
  exit 0
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq gcc libc6-dev curl ca-certificates xz-utils >/dev/null

work="$(mktemp -d /opt/feed-rust-download.XXXXXX)"
trap 'rm -rf "$work"' EXIT
while read -r sha component; do
  archive="$work/$component.tar.xz"
  curl -fsSL --retry 3 -o "$archive" "https://static.rust-lang.org/dist/$component-$VERSION-$TARGET.tar.xz"
  echo "$sha  $archive" | sha256sum -c --quiet -
  mkdir "$work/$component"
  tar -xJf "$archive" -C "$work/$component" --strip-components=1
  "$work/$component/install.sh" --prefix="$PREFIX" --disable-ldconfig >/dev/null
done <<'PINS'
2ff05ff9c08a90861e7f6e4065b40a8b87463b4576186fd613871c3e37f2091f rustc
53f3521949404d595efb06a55c12c5658d4ed1c75566f33d3d514eaebe70bd8a cargo
109b074225bf5b9ea3820890876bfdf4ed161adbfaf94b2b4b5984c85ee5d958 rust-std
PINS
chmod -R a+rX "$PREFIX"
"$PREFIX/bin/rustc" --version
