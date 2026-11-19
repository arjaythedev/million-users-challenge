set -euo pipefail
cd "$(dirname "$0")"
export PATH=/usr/local/go/bin:$PATH CGO_ENABLED=0
mkdir -p bin
[ -f go.sum ] || go mod tidy
go mod download
go build -trimpath -ldflags='-s -w' -o bin/feed .
echo "built bin/feed ($(go version | awk '{print $3}'), no cgo)"
