#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C compiler and the tools build.sh uses: curl to fetch the sources,
# bison and flex for libpq's configure, nasm for ISA-L's x86-64 assembly.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates bison flex nasm
gcc --version | head -1
