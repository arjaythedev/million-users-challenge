#!/usr/bin/env bash
# css-chromium-0x95 toolchain. Run once as root on a clean Ubuntu 24.04 x86_64. Idempotent: installs only what's missing.
#   - apt: build tools + the shared libraries chrome-headless-shell links against
#   - Rust 1.94.0 via rustup into /opt/css-rust   (RUSTUP_HOME=/opt/css-rust/rustup, CARGO_HOME=/opt/css-rust/cargo)
#   - Chrome for Testing chrome-headless-shell linux64 into /opt/css-chromium/  (binary: /opt/css-chromium/chrome-headless-shell)
set -euo pipefail

RUST_VERSION=1.94.0
RUSTUP_VERSION=1.28.2
# Chrome for Testing, exact version (pinned; the newest stable-line build that was >= 2 weeks old when chosen, 2026-10-30:
# 156 had just become stable, 155.0.8059.79 is from the previous stable line). Needs modern CSS: if(), @function, typed attr().
CHROME_VERSION=155.0.8059.79
CHROME_SHA256=4cea4c32b501aa43cc2b81eb45734af991a6a82feded48380d6dfaf96642a1c1
CHROME_URL="https://storage.googleapis.com/chrome-for-testing-public/$CHROME_VERSION/linux64/chrome-headless-shell-linux64.zip"

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  build-essential curl ca-certificates unzip python3 pkg-config \
  libnss3 libnspr4 libatk1.0-0t64 libatk-bridge2.0-0t64 libcups2t64 libdrm2 libxkbcommon0 \
  libxcomposite1 libxdamage1 libxfixes3 libxrandr2 libgbm1 libasound2t64 libpango-1.0-0 libcairo2 \
  libdbus-1-3 libx11-6 libxcb1 libxext6 fonts-liberation

# ---- Rust (rustup, minimal profile) ----
export RUSTUP_HOME=/opt/css-rust/rustup CARGO_HOME=/opt/css-rust/cargo
if ! [ -x "$CARGO_HOME/bin/rustc" ] || [ "$("$CARGO_HOME/bin/rustc" --version | cut -d' ' -f2)" != "$RUST_VERSION" ]; then
  mkdir -p /opt/css-rust
  curl -fsSL "https://static.rust-lang.org/rustup/archive/$RUSTUP_VERSION/x86_64-unknown-linux-gnu/rustup-init" -o /tmp/rustup-init
  chmod +x /tmp/rustup-init
  /tmp/rustup-init -y --no-modify-path --profile minimal --default-toolchain "$RUST_VERSION"
  rm -f /tmp/rustup-init
fi
chmod -R a+rX /opt/css-rust
# build users need to write the registry/cache: they use their own CARGO_HOME for that (see build.sh), tools stay read-only here.
"$CARGO_HOME/bin/rustc" --version

# ---- chrome-headless-shell ----
if [ "$(cat /opt/css-chromium/.version 2>/dev/null || true)" != "$CHROME_VERSION" ]; then
  tmp="$(mktemp -d)"
  curl -fsSL "$CHROME_URL" -o "$tmp/chs.zip"
  echo "$CHROME_SHA256  $tmp/chs.zip" | sha256sum -c -
  unzip -q "$tmp/chs.zip" -d "$tmp"
  rm -rf /opt/css-chromium && mkdir -p /opt/css-chromium
  cp -a "$tmp"/chrome-headless-shell-linux64/. /opt/css-chromium/
  echo "$CHROME_VERSION" > /opt/css-chromium/.version
  rm -rf "$tmp"
fi
chmod -R a+rX /opt/css-chromium
if ldd /opt/css-chromium/chrome-headless-shell | grep -q 'not found'; then
  echo "chrome-headless-shell: missing shared libraries:" >&2; ldd /opt/css-chromium/chrome-headless-shell | grep 'not found' >&2; exit 1
fi
/opt/css-chromium/chrome-headless-shell --version
