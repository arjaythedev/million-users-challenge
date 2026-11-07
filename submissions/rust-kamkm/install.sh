#!/usr/bin/env bash
set -euo pipefail
if ! command -v cc >/dev/null 2>&1 || ! command -v curl >/dev/null 2>&1; then
  apt-get update
  apt-get install -y build-essential curl
fi
export RUSTUP_HOME=/opt/twelve-rust/rustup CARGO_HOME=/opt/twelve-rust/cargo
if [[ ! -x /opt/twelve-rust/cargo/bin/rustup ]]; then
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --no-modify-path --profile minimal --default-toolchain 1.94.0
fi
/opt/twelve-rust/cargo/bin/rustup toolchain install 1.94.0 --profile minimal --no-self-update
