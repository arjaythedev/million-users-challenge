#!/usr/bin/env bash
set -euo pipefail

# Keep the compiler version fixed so benchmark builds are reproducible.
ZIG_VERSION="0.13.0"
case "$(uname -m)" in
  x86_64) ZIG_ARCH="x86_64" ;;
  aarch64|arm64) ZIG_ARCH="aarch64" ;;
  *) echo "unsupported architecture: $(uname -m)" >&2; exit 1 ;;
esac
ARCHIVE="zig-linux-${ZIG_ARCH}-${ZIG_VERSION}.tar.xz"
URL="https://ziglang.org/download/${ZIG_VERSION}/${ARCHIVE}"

# The pinned client libraries need C/assembly tools. OpenSSL headers provide
# JWT verification. Install them on clean Ubuntu machines.
if command -v apt-get >/dev/null 2>&1 && [ "$(id -u)" -eq 0 ]; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends ca-certificates curl xz-utils gcc make nasm bison flex libssl-dev
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
curl --fail --location --retry 3 --silent --show-error "$URL" -o "$TMP/$ARCHIVE"
tar -xJf "$TMP/$ARCHIVE" -C "$TMP"

PREFIX="/opt/zig-${ZIG_VERSION}"
if ! install -d "$PREFIX" 2>/dev/null; then
  PREFIX="${HOME:-/tmp}/.local/zig-${ZIG_VERSION}"
  install -d "$PREFIX"
fi
cp -a "$TMP/zig-linux-${ZIG_ARCH}-${ZIG_VERSION}/." "$PREFIX/"
if install -d /usr/local/bin 2>/dev/null && ln -sfn "$PREFIX/zig" /usr/local/bin/zig 2>/dev/null; then
  :
else
  echo "Zig installed at $PREFIX/zig; add $PREFIX to PATH" >&2
fi
