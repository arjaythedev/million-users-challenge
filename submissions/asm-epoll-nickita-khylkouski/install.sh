#!/usr/bin/env bash
# Build dependencies: only the GNU assembler and linker (binutils). No runtime dependencies.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q binutils
