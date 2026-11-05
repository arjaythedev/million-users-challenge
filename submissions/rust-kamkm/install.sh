#!/usr/bin/env bash
set -euo pipefail
# cc and curl for the build, nasm because ISA-L's x86-64 compression kernels are hand-written
# assembly (build.sh links its static library into the binary).
if ! command -v cc >/dev/null 2>&1 || ! command -v curl >/dev/null 2>&1 || ! command -v nasm >/dev/null 2>&1; then
  apt-get update
  apt-get install -y build-essential curl nasm
fi
export RUSTUP_HOME=/opt/twelve-rust/rustup CARGO_HOME=/opt/twelve-rust/cargo
if [[ ! -x /opt/twelve-rust/cargo/bin/rustup ]]; then
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --no-modify-path --profile minimal --default-toolchain 1.94.0
fi
/opt/twelve-rust/cargo/bin/rustup toolchain install 1.94.0 --profile minimal --no-self-update
