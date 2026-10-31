#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export PATH="${CARGO_HOME:-$HOME/.cargo}/bin:$PATH"
if [ -x /opt/challenge-rust-1.98.1/bin/rustc ]; then
  export PATH="/opt/challenge-rust-1.98.1/bin:$PATH"
fi
if [ "$(rustc --version | cut -d' ' -f2)" != 1.98.1 ]; then
  echo "Rust 1.98.1 required; run install.sh on Ubuntu or install it locally." >&2
  exit 1
fi
# Ship the measured baseline; experiments are not enabled by environment variables.
cargo build --release --locked --features direct-http-task,custom-pg,isolated-writes,least-loaded-db
