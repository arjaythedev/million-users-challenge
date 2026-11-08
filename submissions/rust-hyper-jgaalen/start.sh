#!/usr/bin/env bash
set -euo pipefail
export WRITE_BATCH_SIZE="${WRITE_BATCH_SIZE:-64}"
ulimit -n "$(ulimit -Hn)"
exec "$(cd "$(dirname "$0")" && pwd)/target/release/challenge-rust"
