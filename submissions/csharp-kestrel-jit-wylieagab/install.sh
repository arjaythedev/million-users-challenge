#!/usr/bin/env bash
set -euo pipefail
apt-get update -qq
apt-get install -y -qq curl ca-certificates clang zlib1g-dev libssl-dev libicu74 >/dev/null
SDK=10.0.403
DEST="/opt/million-users-dotnet-$SDK"
if [ ! -x "$DEST/dotnet" ]; then
  ARCHIVE="$(mktemp)"
  trap 'rm -f "$ARCHIVE"' EXIT
  curl -fSL --retry 3 "https://builds.dotnet.microsoft.com/dotnet/Sdk/$SDK/dotnet-sdk-$SDK-linux-x64.tar.gz" -o "$ARCHIVE"
  echo "b7b3d8461cf98b07d2552ce9199702fa3ad010ac2671a5f8cd5136f7534e50f6855ba7bbb02b11494563f1d04156276a31cda1103b900e5eecd0d9473dc7b9c5  $ARCHIVE" | sha512sum -c -
  mkdir -p "$DEST"
  tar -xzf "$ARCHIVE" -C "$DEST"
fi
"$DEST/dotnet" --version
