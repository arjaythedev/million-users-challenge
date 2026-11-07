#!/usr/bin/env bash
# Toolchain for tex-cgi-Emsii3: gcc + libpq (C host and libpq CLI) and pdfTeX. Idempotent.
# Run as root on Ubuntu 24.04.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends gcc libc6-dev libpq-dev texlive-binaries texlive-base
pdftex --version | head -1
