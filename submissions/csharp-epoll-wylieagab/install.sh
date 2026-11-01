#!/usr/bin/env bash
set -euo pipefail

. /etc/os-release
if [[ "${ID:-}" != ubuntu || "${VERSION_ID:-}" != 24.04 || "$(dpkg --print-architecture)" != amd64 ]]; then
  echo 'This installer requires Ubuntu 24.04 amd64.' >&2
  exit 1
fi

LIBPQ_VERSION=18.6-1.pgdg24.04+2
PGDG_FINGERPRINT=B97B0AFCAA1A47F044F244A07FCC7D46ACCC4CF8
for PACKAGE in libpq5 libpq-dev; do
  CURRENT="$(dpkg-query -W -f='${db:Status-Status} ${Version}' "$PACKAGE" 2>/dev/null || true)"
  if [[ "$CURRENT" == installed\ * ]] && dpkg --compare-versions "${CURRENT#installed }" gt "$LIBPQ_VERSION"; then
    echo "$PACKAGE is newer than the validated $LIBPQ_VERSION; use a clean Ubuntu 24.04 host. No downgrade was attempted." >&2
    exit 1
  fi
done

apt-get update -qq
apt-get install -y -qq --no-install-recommends curl ca-certificates gpg libicu74 >/dev/null
TEMP_DIR="$(mktemp -d)"
trap 'rm -rf -- "$TEMP_DIR"' EXIT
install -d -m 0700 "$TEMP_DIR/gnupg"
curl -fSL --retry 3 https://www.postgresql.org/media/keys/ACCC4CF8.asc -o "$TEMP_DIR/pgdg.asc"
# Check every primary key: an unexpected key or an extra key in the bundle fails.
ACTUAL_FINGERPRINT="$(gpg --batch --no-options --homedir "$TEMP_DIR/gnupg" --with-colons --show-keys "$TEMP_DIR/pgdg.asc" |
  awk -F: '$1 == "pub" { primary = 1; next } primary && $1 == "fpr" { print $10; primary = 0 }')"
if [[ "$ACTUAL_FINGERPRINT" != "$PGDG_FINGERPRINT" ]]; then
  echo 'PostgreSQL repository signing key fingerprint mismatch.' >&2
  exit 1
fi
install -d -m 0755 /usr/share/keyrings /etc/apt/sources.list.d
install -m 0644 "$TEMP_DIR/pgdg.asc" /usr/share/keyrings/reactor-pgdg.asc
printf '%s\n' 'deb [arch=amd64 signed-by=/usr/share/keyrings/reactor-pgdg.asc] https://apt.postgresql.org/pub/repos/apt noble-pgdg main' \
  > /etc/apt/sources.list.d/reactor-pgdg.list
apt-get update -qq
apt-get install -y -qq --no-install-recommends "libpq5=$LIBPQ_VERSION" "libpq-dev=$LIBPQ_VERSION" >/dev/null
for PACKAGE in libpq5 libpq-dev; do
  CURRENT="$(dpkg-query -W -f='${db:Status-Status} ${Version}' "$PACKAGE")"
  if [[ "$CURRENT" != "installed $LIBPQ_VERSION" ]]; then
    echo "$PACKAGE installation did not match the required $LIBPQ_VERSION: $CURRENT" >&2
    exit 1
  fi
done
dpkg-query -W -f='${binary:Package}=${Version}\n' libpq5 libpq-dev

SDK=10.0.403
DEST="/opt/million-users-dotnet-$SDK"
if [ ! -x "$DEST/dotnet" ]; then
  ARCHIVE="$TEMP_DIR/dotnet-sdk.tar.gz"
  curl -fSL --retry 3 "https://builds.dotnet.microsoft.com/dotnet/Sdk/$SDK/dotnet-sdk-$SDK-linux-x64.tar.gz" -o "$ARCHIVE"
  echo "b7b3d8461cf98b07d2552ce9199702fa3ad010ac2671a5f8cd5136f7534e50f6855ba7bbb02b11494563f1d04156276a31cda1103b900e5eecd0d9473dc7b9c5  $ARCHIVE" | sha512sum -c -
  mkdir -p "$DEST"
  tar -xzf "$ARCHIVE" -C "$DEST"
fi
"$DEST/dotnet" --version
