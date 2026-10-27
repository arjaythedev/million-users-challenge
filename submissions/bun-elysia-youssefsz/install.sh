#!/usr/bin/env bash
set -euo pipefail
VERSION=1.4.2
apt-get update -qq
apt-get install -y -qq ca-certificates curl unzip
case "$(uname -m)" in
  x86_64)
    if grep -qw avx2 /proc/cpuinfo; then
      target=bun-linux-x64; checksum=36368faef7527875d5ffa52e53cd48021741f2a83eb6208a8dd64068d422a913
    else
      target=bun-linux-x64-baseline; checksum=c678040f14fe0440eb839d37cbd0ce4c051a32da72806ac97de6a6aab6bf728f
    fi ;;
  aarch64) target=bun-linux-aarch64; checksum=54328bbc2d9c8e0c9f892c544d66c57a83b84139e34909e5ee81758f1ac8fda7 ;;
  *) echo 'Unsupported architecture' >&2; exit 1 ;;
esac
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl --fail --location --retry 3 "https://github.com/oven-sh/bun/releases/download/bun-v${VERSION}/${target}.zip" -o "$tmp/bun.zip"
printf '%s  %s\n' "$checksum" "$tmp/bun.zip" | sha256sum -c -
unzip -q "$tmp/bun.zip" -d "$tmp"
install -m 755 "$tmp/$target/bun" /usr/local/bin/bun
bun --version
