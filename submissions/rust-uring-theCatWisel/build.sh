#!/usr/bin/env bash
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

if [ -d /usr/local/rustup ]; then
  export RUSTUP_HOME=/usr/local/rustup
  export RUSTUP_TOOLCHAIN=1.100.0
fi
if [ -d /usr/local/cargo/bin ]; then
  export PATH="/usr/local/cargo/bin:$PATH"
fi

export PYTHONDONTWRITEBYTECODE=1
export CARGO_HOME="$DIR/target/.cargo_home"
export TMPDIR="$DIR/target/.tmp"
PGO_DIR="$DIR/target/.pgo"
rm -rf "$PGO_DIR" "$DIR/scripts/__pycache__"
mkdir -p "$CARGO_HOME" "$TMPDIR" "$PGO_DIR"

PROFDATA=""
if command -v rustc >/dev/null 2>&1; then
  SYSROOT="$(rustc --print sysroot)"
  HOST_TRIPLE="$(rustc -vV | awk '/^host:/ {print $2}')"
  CAND="$SYSROOT/lib/rustlib/$HOST_TRIPLE/bin/llvm-profdata"
  if [ -x "$CAND" ]; then
    PROFDATA="$CAND"
  fi
fi
if [ -z "$PROFDATA" ] && command -v llvm-profdata >/dev/null 2>&1; then
  PROFDATA="$(command -v llvm-profdata)"
fi

HOST_CFG="$(rustc --print cfg -C target-cpu=native 2>/dev/null || true)"
if [ "${FEED_NO_AVX512:-0}" = "1" ]; then
  BASE_FLAGS="-C target-cpu=native -C target-feature=-avx512f,-avx512bw,-avx512cd,-avx512dq,-avx512vl"
elif printf '%s\n' "$HOST_CFG" | grep -q 'target_feature="avx512f"' && ! printf '%s\n' "$HOST_CFG" | grep -q 'target_feature="vaes"'; then
  BASE_FLAGS="-C target-cpu=native -C target-feature=-avx512f,-avx512bw,-avx512cd,-avx512dq,-avx512vl"
else
  BASE_FLAGS="-C target-cpu=native"
fi

LAYOUT_FLAGS="-C link-arg=-Wl,--call-graph-profile-sort=cdsort -C link-arg=-Wl,--icf=safe -C llvm-args=-enable-ext-tsp-block-placement -C llvm-args=-split-machine-functions"

BUILT_PGO=0
if [ -n "$PROFDATA" ] && [ "${FEED_DISABLE_PGO:-0}" != "1" ]; then
  if RUSTFLAGS="$BASE_FLAGS -C profile-generate=$PGO_DIR" \
       cargo build --release --offline --locked --target-dir "$DIR/target/pgo-gen" \
     && python3 -B "$DIR/scripts/pgo_train.py" "$DIR/target/pgo-gen/release/feed" "$PGO_DIR" \
     && "$PROFDATA" merge -o "$PGO_DIR/merged.profdata" "$PGO_DIR"/*.profraw; then
    if RUSTFLAGS="$BASE_FLAGS $LAYOUT_FLAGS -C profile-use=$PGO_DIR/merged.profdata -C llvm-args=-pgo-warn-missing-function" \
         cargo build --release --offline --locked; then
      BUILT_PGO=1
    elif RUSTFLAGS="$BASE_FLAGS -C profile-use=$PGO_DIR/merged.profdata -C llvm-args=-pgo-warn-missing-function" \
         cargo build --release --offline --locked; then
      BUILT_PGO=1
    fi
  fi
  rm -rf "$DIR/target/pgo-gen" "$PGO_DIR"
fi

if [ "$BUILT_PGO" -eq 0 ]; then
  RUSTFLAGS="${RUSTFLAGS:-$BASE_FLAGS}" cargo build --release --offline --locked
fi

cp -f "$DIR/target/release/feed" "$DIR/feed"
chmod 0755 "$DIR/feed"
rm -rf "$TMPDIR" "$PGO_DIR" "$DIR/scripts/__pycache__"
