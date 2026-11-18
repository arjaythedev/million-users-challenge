#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: SBCL from the Ubuntu archive (2.2.9 on 24.04).
set -euo pipefail
apt-get update -q && apt-get install -y -q sbcl
sbcl --version
