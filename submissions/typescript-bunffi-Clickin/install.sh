#!/usr/bin/env bash
# Run once as root on a clean Ubuntu 24.04. Installs Bun (the official release zip, SHA-256 pinned below) into
# /opt/bun, and libdeflate (gzip, called through bun:ffi). x86_64 is the benchmark box; aarch64 is for local runs.
set -euo pipefail
BUN_VERSION=1.4.4
case "$(uname -m)" in   # SHA-256 from the release's SHASUMS256.txt
  x86_64)  NAME=bun-linux-x64     BUN_SHA256=bf8ccbfb2b0b80159e0f9a6d15c55317add8077534dbc6130a3edb52fccb395c ;;
  aarch64) NAME=bun-linux-aarch64 BUN_SHA256=baa6bb36f51d6e58561e02bbea825038a9df454f673d6d776e8bbf86f8c7850e ;;
  *) echo "unsupported architecture $(uname -m)"; exit 1 ;;
esac
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends ca-certificates curl unzip libdeflate0 >/dev/null
DEST=/opt/bun
if [ -x "$DEST/bin/bun" ] && [ "$("$DEST/bin/bun" --version)" = "$BUN_VERSION" ]; then
  echo "bun $BUN_VERSION already in $DEST"
else
  tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
  curl -fsSL -o "$tmp/bun.zip" "https://github.com/oven-sh/bun/releases/download/bun-v$BUN_VERSION/$NAME.zip"
  echo "$BUN_SHA256  $tmp/bun.zip" | sha256sum -c -
  unzip -q "$tmp/bun.zip" -d "$tmp"
  rm -rf "$DEST.new"; mkdir -p "$DEST.new/bin"
  install -m 755 "$tmp/$NAME/bun" "$DEST.new/bin/bun"
  rm -rf "$DEST"; mv "$DEST.new" "$DEST"
fi
"$DEST/bin/bun" --version
