#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends curl gpg ca-certificates
curl -fsSL https://apt.kitware.com/keys/kitware-archive-latest.asc | gpg --dearmor --yes -o /usr/share/keyrings/kitware-archive-keyring.gpg
echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ noble main' \
  > /etc/apt/sources.list.d/kitware.list
apt-get update
apt-get install -y --no-install-recommends gcc g++ cmake ninja-build git liburing-dev
g++ --version
cmake --version
