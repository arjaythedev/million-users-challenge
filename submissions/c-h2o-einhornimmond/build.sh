#!/usr/bin/env bash
# Builds bin/server as a normal user. zig fetches the dependencies pinned in build.zig.zon
# (each with its hash) into the user's cache and compiles them from source: h2o, LibreSSL,
# zlib, libsodium and arnm. libpq, libdeflate and ISA-L come from install.sh. A first build
# takes a few minutes.
set -euo pipefail
cd "$(dirname "$0")"

ZIG="${ZIG:-/opt/zig-0.15.2/zig}"
"$ZIG" build --fetch
"$ZIG" build -Doptimize=ReleaseFast --prefix .
echo "built bin/server"
