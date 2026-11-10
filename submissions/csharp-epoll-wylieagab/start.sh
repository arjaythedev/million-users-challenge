#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export DOTNET_GCHeapHardLimitPercent=0x3c
exec ./out/FeedApi "$@"
