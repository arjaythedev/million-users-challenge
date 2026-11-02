#!/usr/bin/env bash
# Release build (idempotent). Output: target/release/feed-server
set -euo pipefail
cd "$(dirname "$0")"
if [[ -x /opt/twelve-rust/cargo/bin/cargo ]]; then
  export RUSTUP_HOME=/opt/twelve-rust/rustup
  export PATH="/opt/twelve-rust/cargo/bin:$PATH"
else
  export PATH="${CARGO_HOME:-$HOME/.cargo}/bin:$PATH"
fi
export RUSTUP_TOOLCHAIN=1.94.0
# The benchmark boxes are identical, so tune for this CPU.
export RUSTFLAGS="-C target-cpu=native ${RUSTFLAGS:-}"
cargo build --release --locked
