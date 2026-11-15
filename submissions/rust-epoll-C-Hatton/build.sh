#!/usr/bin/env bash
# Release build as a normal user (idempotent). Output: target/release/feed. Dependencies pinned in Cargo.lock.
set -euo pipefail
cd "$(dirname "$0")"
if [[ -x /opt/rust-feed/cargo/bin/cargo ]]; then
  export RUSTUP_HOME=/opt/rust-feed/rustup PATH="/opt/rust-feed/cargo/bin:$PATH"
  export CARGO_HOME="${CARGO_HOME:-$HOME/.cargo}"   # the crate cache stays with the building user
else
  export PATH="$HOME/.cargo/bin:$PATH"               # a rustup install of your own
fi
export RUSTUP_TOOLCHAIN=1.94.0
# The app box builds and runs on the same CPU, so build for it.
export RUSTFLAGS="-C target-cpu=native ${RUSTFLAGS:-}"
cargo build --release --locked
