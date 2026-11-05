#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
# ISA-L 2.31.1, SHA-256 pinned, built as a static library for the igzip compressor (isal.rs).
# Its x86-64 assembly needs nasm (install.sh); other architectures build their own asm fine.
IS=isa-l-2.31.1 ISSHA=e1d5573a4019738243b568ab1e1422e6ab7557c5cae33cc8686944d327ad6bb4
mkdir -p deps
if [[ ! -f deps/$IS/bin/libisa-l.a ]]; then
  if [[ ! -f deps/$IS.tar.gz ]]; then
    curl -fsSL -o deps/$IS.tar.gz https://github.com/intel/isa-l/archive/refs/tags/v2.31.1.tar.gz
  fi
  echo "$ISSHA  deps/$IS.tar.gz" | sha256sum -c -
  rm -rf deps/$IS && tar -xzf deps/$IS.tar.gz -C deps
  make -s -C deps/$IS -f Makefile.unx -j"$(nproc)" lib
  cp deps/$IS/bin/isa-l.a deps/$IS/bin/libisa-l.a   # the name -lisa-l looks for
fi
if [[ -x /opt/twelve-rust/cargo/bin/cargo ]]; then
  export RUSTUP_HOME=/opt/twelve-rust/rustup
  export PATH="/opt/twelve-rust/cargo/bin:$PATH"
else
  export PATH="${CARGO_HOME:-$HOME/.cargo}/bin:$PATH"
fi
export RUSTUP_TOOLCHAIN=1.94.0
export RUSTFLAGS="-C target-cpu=native ${RUSTFLAGS:-}"
cargo build --release --locked
