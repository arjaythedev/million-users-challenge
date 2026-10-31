#!/usr/bin/env bash
set -euo pipefail
GO_VERSION=1.27.3
export PATH=/usr/local/go/bin:$PATH
if ! command -v go >/dev/null || [ "$(go env GOVERSION)" != "go$GO_VERSION" ]; then
  archive="$(mktemp)"
  trap 'rm -f "$archive"' EXIT
  curl -fsSL "https://go.dev/dl/go$GO_VERSION.linux-amd64.tar.gz" -o "$archive"
  echo "65efbc80fdc1fa2cd2631cad5b481777a68e8725f34e89f50fc6ac714e379b40  $archive" | sha256sum -c -
  rm -rf /usr/local/go
  tar -C /usr/local -xzf "$archive"
fi
go version
