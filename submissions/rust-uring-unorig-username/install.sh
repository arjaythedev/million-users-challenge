#!/usr/bin/env bash
# Ubuntu 24.04 x86_64, run as root. Idempotent. Build needs: a C toolchain (linker, and cc for libdeflate) and
# Rust 1.94.0 via rustup in /opt/twelve-rust (the same place the repo's Rust example uses).
set -euo pipefail
if ! command -v cc >/dev/null 2>&1 || ! command -v curl >/dev/null 2>&1; then
  apt-get update
  DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential curl ca-certificates
fi
export RUSTUP_HOME=/opt/twelve-rust/rustup CARGO_HOME=/opt/twelve-rust/cargo
if [[ ! -x /opt/twelve-rust/cargo/bin/rustup ]]; then
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
    | sh -s -- -y --no-modify-path --profile minimal --default-toolchain 1.94.0
fi
/opt/twelve-rust/cargo/bin/rustup toolchain install 1.94.0 --profile minimal --no-self-update
chmod -R a+rX /opt/twelve-rust
