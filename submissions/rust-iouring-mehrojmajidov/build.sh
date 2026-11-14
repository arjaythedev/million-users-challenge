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
FLAGS="-C target-cpu=native ${RUSTFLAGS:-}"
PROFDATA="${PROFDATA:-$(dirname "$CARGO")/../lib/rustlib/x86_64-unknown-linux-gnu/bin/llvm-profdata}"

plain() { RUSTFLAGS="$FLAGS" "$CARGO" build --release --locked --offline; }

# Optional, with FEED_PGO=1 (and install.sh run with it, for llvm-profdata): an instrumented build of the server
# answers a few seconds of the load test's mix of requests on loopback, with a stand-in for Postgres
# (src/train.rs), and the real build is then laid out by what that run executed. If any step fails the build
# is a plain one. Off by default: it measured no faster here.
guided() {
  [ "${FEED_PGO:-0}" = 1 ] && [ -x "$PROFDATA" ] || return 1
  local profile="$PWD/target/profile"
  rm -rf "$profile"
  RUSTFLAGS="$FLAGS -C profile-generate=$profile" \
    "$CARGO" build --release --locked --offline --features train --target-dir target/train || return 1
  # ports below the range the kernel hands to outgoing connections, so the run's own clients cannot take them
  local port trained=0
  for port in 20417 21417 22417; do
    if FEED_TRAIN_SECONDS=5 JWT_SECRET=train HOST=127.0.0.1 PORT=$port EXTRA_PORT=$((port + 1)) \
      timeout 60 target/train/release/feed; then trained=1; break; fi
    rm -f "$profile"/*.profraw
  done
  [ $trained = 1 ] || return 1
  "$PROFDATA" merge -o "$profile/feed.profdata" "$profile" || return 1
  RUSTFLAGS="$FLAGS -C profile-use=$profile/feed.profdata" "$CARGO" build --release --locked --offline
}

if [ "${FEED_PGO:-0}" = 1 ]; then
  guided || { echo "build.sh: no profile, plain build" >&2; plain; }
else
  plain
fi
