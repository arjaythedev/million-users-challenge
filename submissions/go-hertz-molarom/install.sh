#!/usr/bin/env bash
set -euo pipefail
GO_VERSION=1.27.1
export PATH=/usr/local/go/bin:$PATH
if ! command -v go >/dev/null || [ "$(go env GOVERSION)" != "go$GO_VERSION" ]; then
  curl -fsSL "https://go.dev/dl/go$GO_VERSION.linux-amd64.tar.gz" -o /tmp/go.tgz
  rm -rf /usr/local/go && tar -C /usr/local -xzf /tmp/go.tgz && rm -f /tmp/go.tgz
fi
go version
