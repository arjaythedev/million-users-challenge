#!/usr/bin/env bash
set -euo pipefail
ulimit -n "$(ulimit -Hn)"
exec "$(cd "$(dirname "$0")" && pwd)/target/release/feed-rust-luna"
