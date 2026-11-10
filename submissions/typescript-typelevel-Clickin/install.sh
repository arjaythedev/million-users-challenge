#!/usr/bin/env bash
# Install Node.js 22 LTS (official tarball) into /opt/node22. Runs as root on a clean Ubuntu 24.04.
set -euo pipefail
NODE_VERSION=v22.23.3
DEST=/opt/node22
if [ -x "$DEST/bin/node" ] && [ "$("$DEST/bin/node" -v)" = "$NODE_VERSION" ]; then exit 0; fi
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
arch=$(dpkg --print-architecture); case "$arch" in amd64) a=x64;; arm64) a=arm64;; *) a=x64;; esac
file="node-$NODE_VERSION-linux-$a.tar.xz"
curl -fsSL -o "$tmp/$file" "https://nodejs.org/dist/$NODE_VERSION/$file"
curl -fsSL -o "$tmp/SHASUMS256.txt" "https://nodejs.org/dist/$NODE_VERSION/SHASUMS256.txt"
(cd "$tmp" && grep " $file\$" SHASUMS256.txt | sha256sum -c -)
rm -rf "$DEST.new"; mkdir -p "$DEST.new"; tar -xJf "$tmp/$file" -C "$DEST.new" --strip-components=1
rm -rf "$DEST"; mv "$DEST.new" "$DEST"
"$DEST/bin/node" -v
