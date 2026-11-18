#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [[ -x /opt/twelve-rust/cargo/bin/cargo ]]; then
  export RUSTUP_HOME=/opt/twelve-rust/rustup RUSTUP_TOOLCHAIN=1.94.0
  export CARGO_HOME="${CARGO_HOME:-$HOME/.cargo}" PATH="/opt/twelve-rust/cargo/bin:$PATH"
fi
FLAGS="-C target-cpu=native"
if [[ "$(uname -m)" == x86_64 ]]; then FLAGS="$FLAGS -C target-feature=-avx512f"; fi
export RUSTFLAGS="$FLAGS ${RUSTFLAGS:-}"
cargo build --release --locked
