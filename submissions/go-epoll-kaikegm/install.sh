#!/usr/bin/env bash
# Toolchain for go-epoll-kaikegm: Go 1.27.3 (official tarball, sha256-checked).
set -euo pipefail
GO_VERSION=1.27.3
export PATH=/usr/local/go/bin:$PATH
if ! command -v go >/dev/null || [[ "$(go version | awk '{print $3}')" != "go$GO_VERSION" ]]; then
  apt-get update -q && apt-get install -y -q curl jq ca-certificates
  SHA="65efbc80fdc1fa2cd2631cad5b481777a68e8725f34e89f50fc6ac714e379b40"
  curl -fsSLo /tmp/go.tgz "https://go.dev/dl/go$GO_VERSION.linux-amd64.tar.gz"
  echo "$SHA  /tmp/go.tgz" | sha256sum -c -
  rm -rf /usr/local/go && tar -C /usr/local -xzf /tmp/go.tgz && rm /tmp/go.tgz
fi
go version
