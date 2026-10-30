#!/usr/bin/env bash
# The server, in the foreground. Reads DATABASE_URL, JWT_SECRET, HOST, PORT and EXTRA_PORT.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export DOTNET_ROOT=${DOTNET_ROOT:-/usr/share/dotnet}
exec "$here/out/FeedServer"
