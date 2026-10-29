#!/usr/bin/env bash
# Release build (idempotent). Output: target/release/feed. Dependencies are pinned in Cargo.lock.
set -euo pipefail
cd "$(dirname "$0")"
export RUSTUP_HOME=/opt/twelve-rust/rustup RUSTUP_TOOLCHAIN=1.94.0
export CARGO_HOME="${CARGO_HOME:-$HOME/.cargo}" PATH="/opt/twelve-rust/cargo/bin:$PATH"
# The benchmark boxes are identical, so build for this CPU.
export RUSTFLAGS="-C target-cpu=native ${RUSTFLAGS:-}"
cargo build --release --locked
