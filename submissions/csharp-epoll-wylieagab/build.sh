#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
DOTNET=/opt/million-users-dotnet-10.0.403/dotnet
PG_VERSION=18.6
PG_SHA256=983ee554ec53dbeb9b70797bef9fcf4e67e117e7e48ca1463cc80b3ff8e8ff3f
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf -- "$BUILD_DIR"' EXIT
OUT="$PWD/out"
ARCHIVE="postgresql-$PG_VERSION.tar.gz"
(
  cd "$BUILD_DIR"
  curl -fsSL --retry 3 "https://ftp.postgresql.org/pub/source/v$PG_VERSION/$ARCHIVE" -o "$ARCHIVE"
  printf '%s  %s\n' "$PG_SHA256" "$ARCHIVE" | sha256sum -c -
  tar -xzf "$ARCHIVE"
  cd "postgresql-$PG_VERSION"
  ./configure --prefix="$OUT" --with-ssl=openssl --without-icu --without-readline --without-zlib --disable-rpath
  make -s -j"$(nproc)" -C src/backend generated-headers
  make -s -j"$(nproc)" -C src/interfaces/libpq all-shared-lib
)

"$DOTNET" restore FeedApi.csproj -r linux-x64 --locked-mode
"$DOTNET" publish FeedApi.csproj -c Release -r linux-x64 --no-restore -o out
install -m 755 "$BUILD_DIR/postgresql-$PG_VERSION/src/interfaces/libpq/libpq.so.5" "$OUT/libpq.so.5"
"$OUT/FeedApi" --probe-libpq
if [ -f .native/lib/liburing-ffi.so.2 ]; then
  cp -L .native/lib/liburing-ffi.so.2 out/liburing-ffi.so.2
fi
