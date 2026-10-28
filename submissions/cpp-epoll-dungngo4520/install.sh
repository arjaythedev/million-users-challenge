#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04.
set -euo pipefail
apt-get update
apt-get install -y --no-install-recommends g++ libpq-dev libdeflate-dev liburing-dev ca-certificates
