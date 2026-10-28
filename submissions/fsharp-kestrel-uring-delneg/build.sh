#!/usr/bin/env bash
# Builds as a normal user:
#  1. the gzip library out/libgz.so: native/gz.c with libdeflate 1.26 (MIT) and ISA-L 2.31.1 (BSD-3) linked in, both
#     from their pinned, SHA-256-checked source tarballs;
#  2. the server with dotnet publish (ReadyToRun, locked NuGet restore) into out/.
set -euo pipefail
cd "$(dirname "$0")"
DOTNET="${DOTNET:-/usr/local/dotnet/dotnet}"
[ -x "$DOTNET" ] || DOTNET="$(command -v dotnet)"
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 MSBUILDDISABLENODEREUSE=1
LD_VER=1.26
LD_SHA256=bba03fffc5538576213675ce6968fcff6ce2e67d82e4d5febea2d05f9f13cf85
ISAL_VER=2.31.1
ISAL_SHA256=e1d5573a4019738243b568ab1e1422e6ab7557c5cae33cc8686944d327ad6bb4
mkdir -p out deps
fetch() { # fetch <url> <sha256> <dir>: a pinned source tarball, checked, unpacked into deps/
  if [ ! -d "deps/$3" ]; then
    curl -fsSL -o "deps/$3.tar.gz" "$1"
    echo "$2  deps/$3.tar.gz" | sha256sum -c -
    tar -xzf "deps/$3.tar.gz" -C deps
  fi
}
if [ ! -f out/libgz.so ] || [ native/gz.c -nt out/libgz.so ]; then
  fetch "https://github.com/ebiggers/libdeflate/archive/refs/tags/v$LD_VER.tar.gz" $LD_SHA256 "libdeflate-$LD_VER"
  fetch "https://github.com/intel/isa-l/archive/refs/tags/v$ISAL_VER.tar.gz" $ISAL_SHA256 "isa-l-$ISAL_VER"
  # ISA-L's static archive isn't position-independent: link its shared library, found next to libgz.so
  [ -f "deps/isa-l-$ISAL_VER/bin/libisal.so" ] || make -s -C "deps/isa-l-$ISAL_VER" -f Makefile.unx -j"$(nproc)" slib >/dev/null
  cp -L "deps/isa-l-$ISAL_VER/bin/libisal.so" out/libisal.so.2
  L="deps/libdeflate-$LD_VER/lib"
  gcc -O2 -fPIC -shared -fvisibility=hidden -I "deps/libdeflate-$LD_VER" -I "deps/isa-l-$ISAL_VER/include" -o out/libgz.so native/gz.c \
    "$L/deflate_compress.c" "$L/gzip_compress.c" "$L/crc32.c" "$L/adler32.c" "$L/zlib_compress.c" "$L/utils.c" "$L/x86/cpu_features.c" \
    -L out -l:libisal.so.2 -Wl,-rpath,'$ORIGIN'
fi
(cd src && "$DOTNET" publish Server.fsproj -c Release --use-current-runtime --self-contained false -o ../out -p:RestoreLockedMode=true)
"$DOTNET" build-server shutdown > /dev/null 2>&1 || true
