#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

if [ -d "$HOME/.cargo/bin" ]; then
  export PATH="$HOME/.cargo/bin:$PATH"
fi

export RUSTFLAGS="${RUSTFLAGS:--C target-cpu=native}"
cargo build --release --locked

