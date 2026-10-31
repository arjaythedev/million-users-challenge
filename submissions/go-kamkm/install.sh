#!/usr/bin/env bash
set -euo pipefail

GO_VERSION=1.27.1
export PATH=/usr/local/go/bin:$PATH
if ! command -v curl >/dev/null 2>&1; then
  apt-get update
  apt-get install -y ca-certificates curl
fi
if ! command -v go >/dev/null 2>&1 || [ "$(go env GOVERSION)" != "go${GO_VERSION}" ]; then
  curl -fsSL "https://go.dev/dl/go${GO_VERSION}.linux-amd64.tar.gz" -o /tmp/go-luna-go.tgz
  rm -rf /usr/local/go
  tar -C /usr/local -xzf /tmp/go-luna-go.tgz
  rm -f /tmp/go-luna-go.tgz
fi
go version
