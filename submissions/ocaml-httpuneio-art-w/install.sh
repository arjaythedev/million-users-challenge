#!/usr/bin/env bash
# Ubuntu 24.04 x86_64, run as root. Idempotent. Installs what is needed to BUILD this server:
# a C toolchain, opam (which builds OCaml 5.5.0 and the libraries in build.sh) and ISA-L (igzip).
# RUNNING it needs libisal2 (pulled in by libisal-dev).
set -euo pipefail
pkgs=(build-essential curl git unzip rsync bubblewrap pkg-config opam libisal-dev)
missing=()
for p in "${pkgs[@]}"; do dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p"); done
if [ ${#missing[@]} -gt 0 ]; then
  apt-get update
  DEBIAN_FRONTEND=noninteractive apt-get install -y "${missing[@]}"
fi
opam --version
