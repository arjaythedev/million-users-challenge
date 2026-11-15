#!/usr/bin/env bash

set -Eeuo pipefail

if [[ "${EUID}" -ne 0 ]]; then
    echo "ERROR: install.sh must be run as root." >&2
    exit 1
fi

export DEBIAN_FRONTEND=noninteractive

echo "Updating package index..."
apt-get update -qq

echo "Installing build tools and development dependencies..."

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
echo "Verifying compiler and build tools..."

gcc-14 --version
g++-14 --version
cmake --version
ninja --version

echo
echo "Verifying pkg-config dependencies..."

for module in libpqxx libpq liburing openssl; do
    if ! pkg-config --exists "${module}"; then
        echo "ERROR: Missing pkg-config module: ${module}" >&2
        exit 1
    fi

    printf "%s: " "${module}"
    pkg-config --modversion "${module}"
done

echo
echo "Dependencies installed successfully."
