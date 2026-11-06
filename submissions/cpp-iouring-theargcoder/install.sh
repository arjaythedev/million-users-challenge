#!/usr/bin/env bash
set -Eeuo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "install.sh must run as root" >&2
  exit 1
fi

source /etc/os-release

if [[ "${ID}" != "ubuntu" || "${VERSION_ID}" != "24.04" ]]; then
  echo "This submission targets Ubuntu 24.04" >&2
  exit 1
fi

export DEBIAN_FRONTEND=noninteractive

apt-get update

apt-get install -y --no-install-recommends \
  ca-certificates \
  software-properties-common

add-apt-repository -y universe

apt-get update

apt-get install -y --no-install-recommends \
  build-essential \
  gcc-14 \
  g++-14 \
  cmake \
  ninja-build \
  pkg-config \
  libssl-dev \
  libpqxx-dev \
  libpq-dev \
  liburing-dev

echo
echo "Installed compiler:"
gcc-14 --version
g++-14 --version

echo
echo "Installed CMake:"
cmake --version

echo "Dependencies installed successfully."
