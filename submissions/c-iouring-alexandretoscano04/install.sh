#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends gcc-13 libc6-dev binutils make cmake bison flex perl pkg-config libssl-dev curl ca-certificates bzip2 python3
