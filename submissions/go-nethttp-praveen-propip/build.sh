#!/usr/bin/env bash
# Release build of feedapi into ./bin/feedapi (pure Go, CGO_ENABLED=0). Pinned
# dependencies are downloaded from the module proxy (go.sum locks their hashes).
# GOEXPERIMENT=jsonv2 builds with the encoding/json/v2 implementation, the same
# stack our services run in production.
set -euo pipefail
cd "$(dirname "$0")"
export PATH=/usr/local/go/bin:$PATH CGO_ENABLED=0 GOEXPERIMENT=jsonv2
mkdir -p bin
[ -f go.sum ] || go mod tidy
go mod download
go build -trimpath -ldflags='-s -w' -o bin/feedapi .
echo "built bin/feedapi ($(go version | awk '{print $3}'), no cgo, jsonv2)"
