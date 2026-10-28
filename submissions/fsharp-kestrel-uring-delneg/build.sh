#!/usr/bin/env bash
# Builds as a normal user:
#  1. libdeflate 1.26 (gzip, MIT) from its pinned, SHA-256-checked source tarball into out/libdeflate.so;
#  2. the server with dotnet publish (ReadyToRun, locked NuGet restore) into out/.
set -euo pipefail
cd "$(dirname "$0")"
DOTNET="${DOTNET:-/usr/local/dotnet/dotnet}"
[ -x "$DOTNET" ] || DOTNET="$(command -v dotnet)"
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 MSBUILDDISABLENODEREUSE=1
LD_VER=1.26
LD_SHA256=bba03fffc5538576213675ce6968fcff6ce2e67d82e4d5febea2d05f9f13cf85
mkdir -p out native
if [ ! -f out/libdeflate.so ]; then
  if [ ! -d "native/libdeflate-$LD_VER" ]; then
    curl -fsSL -o native/libdeflate.tar.gz "https://github.com/ebiggers/libdeflate/archive/refs/tags/v$LD_VER.tar.gz"
    echo "$LD_SHA256  native/libdeflate.tar.gz" | sha256sum -c -
    tar -xzf native/libdeflate.tar.gz -C native
  fi
  L="native/libdeflate-$LD_VER/lib"
  gcc -O2 -fPIC -shared -I "native/libdeflate-$LD_VER" -o out/libdeflate.so \
    "$L/deflate_compress.c" "$L/gzip_compress.c" "$L/crc32.c" "$L/adler32.c" "$L/zlib_compress.c" "$L/utils.c" "$L/x86/cpu_features.c"
fi
(cd src && "$DOTNET" publish Server.fsproj -c Release --use-current-runtime --self-contained false -o ../out -p:RestoreLockedMode=true)
"$DOTNET" build-server shutdown > /dev/null 2>&1 || true
