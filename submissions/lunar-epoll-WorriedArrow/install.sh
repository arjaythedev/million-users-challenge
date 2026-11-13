#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C toolchain (Lunar links with `cc`) and the Lunar 0.2.0 compiler,
# from its official release tarball, SHA-256 pinned, unpacked into /opt/lunar-0.2.0.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates
V=0.2.0 SHA=6bd494e1983448d99758f3e519efa5a173eca64eb3896ed7adb2d64fdb14f41e
T=lunar-$V-x86_64-unknown-linux-gnu.tar.gz
curl -fsSL -o /tmp/$T https://github.com/lunarln/lunar/releases/download/v$V/$T
echo "$SHA  /tmp/$T" | sha256sum -c -
rm -rf /opt/lunar-$V && mkdir -p /opt/lunar-$V
tar -xzf /tmp/$T -C /opt/lunar-$V --strip-components=1
rm /tmp/$T
/opt/lunar-$V/bin/lunar --version
