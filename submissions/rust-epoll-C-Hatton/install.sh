#!/usr/bin/env bash
# Ubuntu 24.04 x86_64, run once as root. Idempotent. Installs a C toolchain (linker, and cc for libdeflate's
# vendored C source) and Rust 1.94.0 via rustup into /opt/rust-feed, readable by every user.
set -euo pipefail
if ! command -v cc >/dev/null 2>&1 || ! command -v curl >/dev/null 2>&1; then
  apt-get update
  DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential curl ca-certificates
fi
export RUSTUP_HOME=/opt/rust-feed/rustup CARGO_HOME=/opt/rust-feed/cargo
if [[ ! -x /opt/rust-feed/cargo/bin/rustup ]]; then
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
    | sh -s -- -y --no-modify-path --profile minimal --default-toolchain 1.94.0
fi
/opt/rust-feed/cargo/bin/rustup toolchain install 1.94.0 --profile minimal --no-self-update
chmod -R a+rX /opt/rust-feed
