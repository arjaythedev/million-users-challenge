#!/usr/bin/env bash
# Toolchain: gcc and liburing (both from the Ubuntu 24.04 archive, so the distro release is the pin).
# Idempotent. Run once as root.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential liburing-dev
gcc --version | head -1
