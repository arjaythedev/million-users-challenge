#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

export RUSTFLAGS="${RUSTFLAGS:--C target-cpu=native}"
if [ -x "/opt/tvcc-rust/cargo/bin/cargo" ]; then
  export RUSTUP_HOME="/opt/tvcc-rust/rustup"
  export CARGO_HOME="${CARGO_HOME:-$HOME/.cargo}"
  export PATH="/opt/tvcc-rust/cargo/bin:$PATH"
  /opt/tvcc-rust/cargo/bin/cargo +1.94.0 build --release --locked
else
  cargo build --release --locked
fi
