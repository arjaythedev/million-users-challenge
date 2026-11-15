#!/usr/bin/env bash
# Release build (idempotent): target/release/feed.
# PROFILING=1 builds target/profiling/feed instead: the same optimized code plus frame pointers and line tables,
# for `perf record -g` (start.sh runs it when PROFILING=1 too).
set -euo pipefail
cd "$(dirname "$0")"
if [[ -x /opt/twelve-rust/cargo/bin/cargo ]]; then
  export RUSTUP_HOME=/opt/twelve-rust/rustup
  export PATH="/opt/twelve-rust/cargo/bin:$PATH"
else
  export PATH="${CARGO_HOME:-$HOME/.cargo}/bin:$PATH"
fi
export RUSTUP_TOOLCHAIN=1.94.0
# Built on the box it runs on, so tune for that CPU. Same flags as .cargo/config.toml, which plain cargo commands
# use; they're repeated here because RUSTFLAGS (set for PROFILING=1) replaces the config's flags.
export RUSTFLAGS="-C target-cpu=native ${RUSTFLAGS:-}"
if [[ "${PROFILING:-0}" = 1 ]]; then
  RUSTFLAGS="$RUSTFLAGS -C force-frame-pointers=yes" cargo build --profile profiling --locked
else
  cargo build --release --locked
fi
