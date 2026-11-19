#!/usr/bin/env bash
# Release build (idempotent). Output: target/release/feed. The crate has no dependencies, so nothing is downloaded.
set -euo pipefail
cd "$(dirname "$0")"
# install.sh puts the pinned toolchain here. CARGO=... overrides it for local work on another machine.
CARGO="${CARGO:-/opt/feed-rust/bin/cargo}"
export PATH="$(dirname "$CARGO"):$PATH"
# cargo's own files stay inside this folder too
export CARGO_HOME="$PWD/target/cargo-home"
# The server is built on the box it runs on, so build for that CPU.
export RUSTFLAGS="-C target-cpu=native ${RUSTFLAGS:-}"
"$CARGO" build --release --locked --offline
