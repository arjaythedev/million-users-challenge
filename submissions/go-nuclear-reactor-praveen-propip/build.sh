#!/usr/bin/env bash
# Release build of feedreactor into ./bin/feedreactor (pure Go, CGO_ENABLED=0).
# Pinned dependencies are downloaded from the module proxy (go.sum locks hashes).
set -euo pipefail
cd "$(dirname "$0")"
export PATH=/usr/local/go/bin:$PATH CGO_ENABLED=0
mkdir -p bin
[ -f go.sum ] || go mod tidy
go mod download
go build -trimpath -ldflags='-s -w' -o bin/feedreactor .
echo "built bin/feedreactor ($(go version | awk '{print $3}'), no cgo)"
