#!/usr/bin/env bash
# Builds as a normal user: dotnet publish (locked NuGet restore) into out/.
set -euo pipefail
cd "$(dirname "$0")"
DOTNET="${DOTNET:-/usr/local/dotnet/dotnet}"
[ -x "$DOTNET" ] || DOTNET="$(command -v dotnet)"
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 MSBUILDDISABLENODEREUSE=1
(cd src && "$DOTNET" publish Server.fsproj -c Release --use-current-runtime --self-contained false -o ../out -p:RestoreLockedMode=true)
"$DOTNET" build-server shutdown > /dev/null 2>&1 || true
