#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: the C3 compiler (official static tarball, v0.8.5_1, SHA-256 pinned)
# into /opt/c3, plus the C/assembly toolchain used by the link step and ISA-L.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates nasm
C3SHA=939135bc6fbe9d67bf3b19c08146b93ab10996718aafff692294ae6c0b3a3f88
curl -fsSL -o /tmp/c3.tar.gz https://github.com/c3lang/c3c/releases/download/v0.8.5_1/c3-linux-static.tar.gz
echo "$C3SHA  /tmp/c3.tar.gz" | sha256sum -c -
rm -rf /opt/c3 && mkdir -p /opt/c3 && tar -xzf /tmp/c3.tar.gz -C /opt/c3 --strip-components=1 && rm -f /tmp/c3.tar.gz
ln -sf /opt/c3/c3c /usr/local/bin/c3c
c3c --version
