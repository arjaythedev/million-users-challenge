#!/usr/bin/env bash
set -euo pipefail

GO_VERSION=1.27.1
export PATH=/usr/local/go/bin:$PATH
# curl fetches the toolchain and ISA-L; gcc, make and nasm build ISA-L and the
# cgo wrapper around it.
missing=""
for tool in curl gcc make nasm; do
  command -v "$tool" >/dev/null 2>&1 || missing=1
done
if [ -n "$missing" ]; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends ca-certificates curl gcc libc6-dev make nasm
fi
if ! command -v go >/dev/null 2>&1 || [ "$(go env GOVERSION)" != "go${GO_VERSION}" ]; then
  curl -fsSL "https://go.dev/dl/go${GO_VERSION}.linux-amd64.tar.gz" -o /tmp/go-epoll-kamkm-go.tgz
  rm -rf /usr/local/go
  tar -C /usr/local -xzf /tmp/go-epoll-kamkm-go.tgz
  rm -f /tmp/go-epoll-kamkm-go.tgz
fi
go version
