#!/usr/bin/env bash
set -euo pipefail
umask 022

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  autoconf \
  automake \
  build-essential \
  ca-certificates \
  curl \
  libisal-dev \
  libtool \
  make \
  nasm \
  pkg-config \
  python3

if [ ! -f /usr/local/lib/libisal.a ]; then
  ISAL_VER="2.31.1"
  ISAL_SHA256="e1d5573a4019738243b568ab1e1422e6ab7557c5cae33cc8686944d327ad6bb4"
  ISAL_CFLAGS="-O3 -march=native -fPIC"
  HOST_DEFS="$(cc -march=native -dM -E - < /dev/null 2>/dev/null || true)"
  if printf '%s\n' "$HOST_DEFS" | grep -q '__AVX512F__' && ! printf '%s\n' "$HOST_DEFS" | grep -q '__VAES__'; then
    ISAL_CFLAGS="$ISAL_CFLAGS -mno-avx512f -mprefer-vector-width=256"
  fi
  WORK="$(mktemp -d /tmp/isal-build.XXXXXX)"
  trap 'rm -rf "$WORK"' EXIT
  curl --proto '=https' --tlsv1.2 -fsSL \
    "https://github.com/intel/isa-l/archive/refs/tags/v${ISAL_VER}.tar.gz" \
    -o "$WORK/isa-l.tar.gz"
  echo "${ISAL_SHA256}  $WORK/isa-l.tar.gz" | sha256sum -c -
  tar -xzf "$WORK/isa-l.tar.gz" -C "$WORK"
  pushd "$WORK/isa-l-${ISAL_VER}" >/dev/null
  ./autogen.sh
  CFLAGS="$ISAL_CFLAGS" ./configure --prefix=/usr/local --enable-static --disable-shared
  make -j"$(nproc)"
  make install
  popd >/dev/null
  rm -rf "$WORK"
  trap - EXIT
fi

export RUSTUP_HOME=/usr/local/rustup
export CARGO_HOME=/usr/local/cargo
RUST_VER="1.100.0"
if [ ! -x /usr/local/cargo/bin/cargo ] || [ ! -x /usr/local/cargo/bin/rustc ]; then
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
    | sh -s -- -y --profile minimal --default-toolchain "${RUST_VER}" --no-modify-path
fi
/usr/local/cargo/bin/rustup toolchain install "${RUST_VER}" --profile minimal
/usr/local/cargo/bin/rustup default "${RUST_VER}"
/usr/local/cargo/bin/rustup component add llvm-tools-preview --toolchain "${RUST_VER}"

chmod -R a+rX /usr/local/rustup /usr/local/cargo /usr/local/include/isa-l* /usr/local/lib/libisal* 2>/dev/null || true
chmod -R a+rX /usr/local/rustup /usr/local/cargo
for bin in cargo rustc rustdoc rustup; do
  if [ -x "/usr/local/cargo/bin/$bin" ]; then
    ln -sf "/usr/local/cargo/bin/$bin" "/usr/local/bin/$bin"
  fi
done
