#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04:
#   - ocaml, dune, libppx-deriving-ocaml-dev, libfindlib-ocaml-dev: builds Zaneham/plankalkul-compiler from source
#   - build-essential, curl, ca-certificates: C toolchain & source downloads
#   - bison, flex: PostgreSQL 18.6 static libpq configure
#   - nasm: Intel ISA-L 2.31.1 x86-64 igzip assembly
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  build-essential curl ca-certificates bison flex nasm \
  ocaml dune libppx-deriving-ocaml-dev libfindlib-ocaml-dev
ocamlc -version
dune --version
gcc --version | head -1
