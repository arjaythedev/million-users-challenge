#!/usr/bin/env bash
set -euo pipefail

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    SUDO="sudo"
fi

export DEBIAN_FRONTEND=noninteractive
$SUDO apt-get update -y
$SUDO apt-get install -y --no-install-recommends curl ca-certificates tar libisal2 libisal-dev

if ! command -v javac >/dev/null 2>&1 || ! javac -version 2>&1 | grep -qE "(25|26)(\.|$| )"; then
    if apt-cache show openjdk-25-jdk-headless >/dev/null 2>&1; then
        $SUDO apt-get install -y --no-install-recommends openjdk-25-jdk-headless
    else
        echo "Installing Eclipse Temurin JDK 25..."
        $SUDO mkdir -p /opt/jdk-25
        curl -fsSL "https://api.adoptium.net/v3/binary/latest/25/ga/linux/x64/jdk/hotspot/normal/eclipse?project=jdk" \
            | $SUDO tar -xz -C /opt/jdk-25 --strip-components=1
        $SUDO ln -sf /opt/jdk-25/bin/java /usr/local/bin/java
        $SUDO ln -sf /opt/jdk-25/bin/javac /usr/local/bin/javac
    fi
fi

if [ -x /opt/jdk-25/bin/java ]; then
    /opt/jdk-25/bin/java -version
else
    java -version
fi
