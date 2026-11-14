#!/usr/bin/env bash
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -x /opt/pulse-rust/cargo/bin/cargo ]; then
  export RUSTUP_HOME=/opt/pulse-rust/rustup
  export PATH="/opt/pulse-rust/cargo/bin:$PATH"
fi
export CARGO_HOME="$DIR/.cargo-home"
case "$(uname -m)" in
  x86_64) FLAGS="-C target-cpu=cascadelake -C target-feature=-avx512f" ;;
  *) FLAGS="-C target-cpu=native" ;;
esac
if [ "${PULSE_PROFILE:-0}" = 1 ]; then
  FLAGS="$FLAGS -C force-frame-pointers=yes -g"
fi
cd "$DIR"
RUSTFLAGS="$FLAGS" cargo build --release --locked --quiet
