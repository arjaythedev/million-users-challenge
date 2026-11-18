#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export PATH=/usr/local/go/bin:$PATH
export GOTOOLCHAIN=go1.27.1
mkdir -p bin deps

# Responses are compressed with ISA-L (gzip_isal.go), built from a pinned
# release. Without the "isal" tag the server uses its pure-Go encoder
# (gzipfast.go) instead.
ISAL_VERSION=2.31.1
ISAL_SHA256=e1d5573a4019738243b568ab1e1422e6ab7557c5cae33cc8686944d327ad6bb4
ISAL_DIR="deps/isa-l-${ISAL_VERSION}"
if [ ! -f "$ISAL_DIR/bin/isa-l.a" ]; then
  archive="deps/isa-l-${ISAL_VERSION}.tar.gz"
  if [ ! -f "$archive" ]; then
    curl --fail --location --retry 3 --silent --show-error \
      "https://github.com/intel/isa-l/archive/refs/tags/v${ISAL_VERSION}.tar.gz" -o "$archive.tmp"
    mv "$archive.tmp" "$archive"
  fi
  echo "$ISAL_SHA256  $archive" | sha256sum --check --quiet
  rm -rf "$ISAL_DIR"
  tar -xzf "$archive" -C deps
  make -s -C "$ISAL_DIR" -f Makefile.unx -j"$(getconf _NPROCESSORS_ONLN)" lib
fi

export CGO_ENABLED=1
export CGO_CFLAGS="-O2 -I$PWD/$ISAL_DIR/include"
export CGO_LDFLAGS="$PWD/$ISAL_DIR/bin/isa-l.a"
go mod download
go build -trimpath -tags isal -ldflags='-s -w' -o bin/feed-go-epoll-kamkm .
echo "built bin/feed-go-epoll-kamkm ($(go version | awk '{print $3}'), ISA-L ${ISAL_VERSION})"
