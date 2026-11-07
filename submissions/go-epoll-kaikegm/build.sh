#!/usr/bin/env bash
# Release build of go-epoll-kaikegm into ./bin/go-epoll-kaikegm (pure Go, CGO_ENABLED=0).
set -euo pipefail
cd "$(dirname "$0")"
export PATH=/usr/local/go/bin:$PATH CGO_ENABLED=0
mkdir -p bin
[ -f go.sum ] || go mod tidy
go mod download
go build -trimpath -ldflags='-s -w' -o bin/go-epoll-kaikegm .
echo "built bin/go-epoll-kaikegm ($(go version | awk '{print $3}'), no cgo)"
