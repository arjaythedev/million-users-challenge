#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04. GNU binutils (as, ld) is the whole toolchain.
set -euo pipefail
apt-get update -q && apt-get install -y -q binutils
as --version | head -1
