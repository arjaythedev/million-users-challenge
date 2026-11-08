#!/usr/bin/env bash
# Install ponyc (the official release build for Ubuntu 24.04) into /opt/ponyc and corral (Pony's dependency
# manager) into /opt/corral, both SHA-512 pinned, plus the C toolchain, OpenSSL and libdeflate (gzip) that the
# program links with.
# Idempotent; run as root. x86_64 is the benchmark box; aarch64 is for local runs (Docker on an ARM Mac).
set -euo pipefail
PONYC_VERSION=0.77.1
CORRAL_VERSION=0.9.2
case "$(uname -m)" in   # SHA-512 from the releases' .sha512 files
  x86_64)  ARCH=x86-64
           PONYC_SHA512=f31ed3a8f2269a22ab72fbd47cddcd0cbe4e7ce48911dbb5add4ab601e9e3359762fb4e8b592f747c385acc632984818284ea5435d5d7ba1ab7d21944e7f9d82
           CORRAL_SHA512=f442ec5d1ddf311038130361a3d5917f192e5ba3252c626ae6c05e9f643209122bea2f84e64689b5b6e06292ecbc365a10a061d8b0dd7417d8d4a685d1dc5d3c ;;
  aarch64) ARCH=arm64
           PONYC_SHA512=34c3e0836cd212d68e81d6cfdcd7b601472dce390e27f8c932154ee68397ba5536ba4cdc61a0008f39f3b1144d4bcf59bc7767b078edd4105488699a0084d094
           CORRAL_SHA512=cbf98070a72081d28c15b09ed97144a20dba0aded1d384bd2b2a6b206fd88cd817e537e0601cb9e1753121bd07cab8ceaff0737ee87cc90ddb9c9abb86d52a1b ;;
  *) echo "unsupported architecture $(uname -m)"; exit 1 ;;
esac

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential libssl-dev libdeflate-dev git curl ca-certificates >/dev/null
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

# install <name> <version> <url> <sha512> <dest>
install_tarball() {
  if [ "$(cat "$5/RELEASE_VERSION" 2>/dev/null)" = "$2" ]; then echo "$1 $2 already in $5"; return; fi
  curl -fsSL -o "$tmp/$1.tar.gz" "$3"
  echo "$4  $tmp/$1.tar.gz" | sha512sum -c -
  rm -rf "$5" && mkdir -p "$5"
  tar -xzf "$tmp/$1.tar.gz" -C "$5" --strip-components=1
  echo "$2" > "$5/RELEASE_VERSION"
}
install_tarball ponyc "$PONYC_VERSION" \
  "https://github.com/ponylang/ponyc/releases/download/$PONYC_VERSION/ponyc-$ARCH-unknown-linux-ubuntu24.04.tar.gz" \
  "$PONYC_SHA512" /opt/ponyc
install_tarball corral "$CORRAL_VERSION" \
  "https://github.com/ponylang/corral/releases/download/$CORRAL_VERSION/corral-$ARCH-unknown-linux.tar.gz" \
  "$CORRAL_SHA512" /opt/corral

/opt/ponyc/bin/ponyc --version
/opt/corral/bin/corral version
