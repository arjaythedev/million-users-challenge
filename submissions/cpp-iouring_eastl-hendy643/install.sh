#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends curl gpg ca-certificates gcc g++ \
  cmake ninja-build git liburing-dev cmake extra-cmake-modules
g++ --version
cmake --version
