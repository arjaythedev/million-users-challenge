#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C compiler and the kernel headers (linux-libc-dev comes with it).
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential
gcc --version | head -1
