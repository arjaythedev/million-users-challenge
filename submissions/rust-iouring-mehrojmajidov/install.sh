#!/usr/bin/env bash
# Ubuntu 24.04 x86_64, run once as root. Idempotent.
# Installs a C linker (apt) and Rust 1.99.1 into /opt/feed-rust from the official archives, each checked
# against the SHA-256 pinned below: the compiler, cargo, the standard library, and llvm-tools for
# `llvm-profdata`, which build.sh's profile-guided build uses. The crate itself has no dependencies.
set -euo pipefail
VERSION=1.99.1
TARGET=x86_64-unknown-linux-gnu
PREFIX=/opt/feed-rust

have_rust() { [ -x "$PREFIX/bin/cargo" ] && "$PREFIX/bin/rustc" --version 2>/dev/null | grep -q "^rustc $VERSION "; }
have_tools() { [ -x "$PREFIX/lib/rustlib/$TARGET/bin/llvm-profdata" ]; }
if have_rust && have_tools; then
  echo "Rust $VERSION is already in $PREFIX"
  exit 0
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq gcc libc6-dev curl ca-certificates xz-utils >/dev/null

# decided once, before anything is installed
rust=no; tools=no
if have_rust; then rust=yes; fi
if have_tools; then tools=yes; fi
work="$(mktemp -d /opt/feed-rust-download.XXXXXX)"
trap 'rm -rf "$work"' EXIT
while read -r sha component; do
  # an earlier install of this toolchain may lack only the tools
  if [ "$component" = llvm-tools ] && [ $tools = yes ]; then continue; fi
  if [ "$component" != llvm-tools ] && [ $rust = yes ]; then continue; fi
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
8429c5117debfb45c22cf2f97712ef00e0e94976c96cf92f661974a458ca7e84 llvm-tools
PINS
chmod -R a+rX "$PREFIX"
"$PREFIX/bin/rustc" --version
