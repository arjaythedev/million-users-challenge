#!/usr/bin/env bash
# Clean Ubuntu 24.04 x86_64, as root. Only installs the pinned JDK; no native build tools or libraries.
set -euo pipefail
version=27.0.1.0.2+1
url='https://github.com/adoptium/temurin27-binaries/releases/download/jdk-27.0.1.0.2%2B1/OpenJDK27U-jdk_x64_linux_hotspot_27.0.1.0.2_1.tar.gz'
sha=30cb129f6cf299c852cec0feca9ea2a596d2ee9f26d8605d3a253951f6c8b20f
if [ -x /opt/jdk27/bin/java ] && /opt/jdk27/bin/java -version 2>&1 | grep -Fq "build $version)"; then
  /opt/jdk27/bin/java -version
  exit 0
fi
if ! command -v curl >/dev/null; then
  apt-get update
  apt-get install -y --no-install-recommends curl ca-certificates
fi
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
curl -fsSL "$url" -o "$tmp/jdk.tar.gz"
echo "$sha  $tmp/jdk.tar.gz" | sha256sum -c -
mkdir "$tmp/jdk"
tar xzf "$tmp/jdk.tar.gz" -C "$tmp/jdk" --strip-components=1
rm -rf /opt/jdk27
mv "$tmp/jdk" /opt/jdk27
/opt/jdk27/bin/java -version
