#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
# Reserve RAM for kernel sockets and unmanaged runtime/transport buffers.
# Runtime environment numeric values are hexadecimal: 0x3c = 60 percent.
export DOTNET_GCHeapHardLimitPercent=0x3c
exec ./out/FeedApi
