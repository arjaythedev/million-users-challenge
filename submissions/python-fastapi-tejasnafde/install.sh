#!/usr/bin/env bash
# Run as root on Ubuntu 24.04. Idempotent: installs Python 3.12 + venv from Ubuntu only if missing.
set -euo pipefail
if python3 -c 'import sys, venv, ensurepip; sys.exit(sys.version_info[:2] != (3, 12))' 2>/dev/null; then
  echo "python3 $(python3 -V) with venv already installed"
else
  export DEBIAN_FRONTEND=noninteractive
  apt-get update
  apt-get install -y --no-install-recommends python3 python3-venv
fi
python3 --version
