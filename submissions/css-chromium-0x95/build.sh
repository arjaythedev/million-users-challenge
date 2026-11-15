#!/usr/bin/env bash
# css-chromium-0x95 build, as a normal user: generate web/server.css (gen/build.py), then compile the Rust front
# -> target/release/css-front.
set -euo pipefail
cd "$(dirname "$0")"

PYTHONDONTWRITEBYTECODE=1 python3 gen/build.py

# Toolchain: rustup-installed Rust 1.94.0 in /opt/css-rust (read-only, see install.sh); build users keep their own
# CARGO_HOME for the registry cache.
export RUSTUP_HOME="${RUSTUP_HOME:-/opt/css-rust/rustup}"
if [[ -z "${CARGO_HOME:-}" || ! -w "$CARGO_HOME" ]]; then export CARGO_HOME="$HOME/.cargo"; fi
export PATH="/opt/css-rust/cargo/bin:$PATH"
export RUSTUP_TOOLCHAIN=1.94.0
# the benchmark boxes are identical, so tune for this CPU
export RUSTFLAGS="-C target-cpu=native ${RUSTFLAGS:-}"
cargo build --release --locked
