#!/usr/bin/env bash
# Ubuntu 24.04 x86_64, run once as root. Idempotent. Installs what is needed to BUILD this server: a C toolchain
# (the linker, and cc for the libdeflate and mimalloc sources) and Rust 1.94.0 via rustup in /opt/twelve-rust
# (the same location as examples/rust). Running it needs nothing extra: the binary links only glibc.
set -euo pipefail
if ! command -v cc >/dev/null 2>&1 || ! command -v curl >/dev/null 2>&1; then
  apt-get update
  apt-get install -y build-essential curl ca-certificates
fi
export RUSTUP_HOME=/opt/twelve-rust/rustup CARGO_HOME=/opt/twelve-rust/cargo
if [[ ! -x /opt/twelve-rust/cargo/bin/rustup ]]; then
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
    | sh -s -- -y --no-modify-path --profile minimal --default-toolchain 1.94.0
fi
/opt/twelve-rust/cargo/bin/rustup toolchain install 1.94.0 --profile minimal --no-self-update
/opt/twelve-rust/cargo/bin/cargo +1.94.0 --version
