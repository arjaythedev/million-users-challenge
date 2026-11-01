#!/usr/bin/env bash
# Toolchain for feedreactor: Go 1.27.3 (go.mod pins toolchain go1.27.3). Pure
# Go (no cgo), so no gcc needed. Idempotent; installs only what's missing.
# Run as root on clean Ubuntu 24.04 x86_64.
set -euo pipefail
GO_VERSION=1.27.3
export PATH=/usr/local/go/bin:$PATH
# Clean minimal images ship without curl; CI runners have it. Be self-sufficient.
if ! command -v curl >/dev/null; then
  apt-get update -qq && apt-get install -y -qq curl ca-certificates
fi
if ! command -v go >/dev/null || [ "$(go env GOVERSION)" != "go$GO_VERSION" ]; then
  curl -fsSL "https://go.dev/dl/go$GO_VERSION.linux-amd64.tar.gz" -o /tmp/go.tgz
  rm -rf /usr/local/go && tar -C /usr/local -xzf /tmp/go.tgz && rm -f /tmp/go.tgz
fi
go version
