#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C compiler, the tools build.sh uses (curl for the sources, bison and
# flex for libpq's configure, nasm for ISA-L's x86-64 assembly) and git + cmake to build the Goose compiler.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates bison flex nasm git cmake
gcc --version | head -1
cmake --version | head -1
