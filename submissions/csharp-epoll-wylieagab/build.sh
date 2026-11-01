#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
DOTNET=/opt/million-users-dotnet-10.0.403/dotnet
"$DOTNET" restore FeedApi.csproj -r linux-x64 --locked-mode
"$DOTNET" publish FeedApi.csproj -c Release -r linux-x64 --no-restore -o out
if [ -f .native/lib/liburing-ffi.so.2 ]; then
  cp -L .native/lib/liburing-ffi.so.2 out/liburing-ffi.so.2
fi
