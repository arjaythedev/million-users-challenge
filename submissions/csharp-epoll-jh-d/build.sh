#!/usr/bin/env bash
# Release build of the server into out/ (framework-dependent; install.sh provides the runtime).
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DOTNET_NOLOGO=1 DOTNET_CLI_TELEMETRY_OPTOUT=1 \
  dotnet publish "$here/FeedServer.csproj" --configuration Release --output "$here/out" --verbosity quiet
