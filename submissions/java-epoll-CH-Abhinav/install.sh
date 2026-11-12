#!/usr/bin/env bash
set -euo pipefail

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    SUDO="sudo"
fi

export DEBIAN_FRONTEND=noninteractive
$SUDO apt-get update -y
$SUDO apt-get install -y --no-install-recommends curl ca-certificates tar libisal2 libisal-dev

has_jdk_25() {
    local c
    for c in /opt/jdk-25/bin/javac /usr/lib/jvm/java-25-*/bin/javac /usr/lib/jvm/*-25*/bin/javac; do
        if [ -x "$c" ]; then
            return 0
        fi
    done
    if command -v javac >/dev/null 2>&1 && javac -version 2>&1 | grep -qE "^javac (25|26)(\.|$)"; then
        return 0
    fi
    return 1
}

if ! has_jdk_25; then
    if apt-cache show openjdk-25-jdk-headless 2>/dev/null | grep -q "^Package: openjdk-25-jdk-headless"; then
        $SUDO apt-get install -y --no-install-recommends openjdk-25-jdk-headless
    else
        echo "Installing Eclipse Temurin JDK 25..."
        ARCH="x64"
        if [ "$(uname -m)" = "aarch64" ]; then
            ARCH="aarch64"
        fi
        $SUDO mkdir -p /opt/jdk-25
        curl -fsSL "https://api.adoptium.net/v3/binary/latest/25/ga/linux/$ARCH/jdk/hotspot/normal/eclipse?project=jdk" \
            | $SUDO tar -xz -C /opt/jdk-25 --strip-components=1
        $SUDO ln -sf /opt/jdk-25/bin/java /usr/local/bin/java
        $SUDO ln -sf /opt/jdk-25/bin/javac /usr/local/bin/javac
    fi
fi

JAVA_BIN="java"
for candidate in /opt/jdk-25/bin/java /usr/lib/jvm/java-25-*/bin/java /usr/lib/jvm/*-25*/bin/java; do
    if [ -x "$candidate" ]; then
        JAVA_BIN="$candidate"
        break
    fi
done
"$JAVA_BIN" -version
