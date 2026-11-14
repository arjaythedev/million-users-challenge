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
        echo "Installing pinned Eclipse Temurin JDK 25..."
        JDK="jdk-25.0.4.1+1"
        case "$(uname -m)" in
            x86_64)  ARCH="x64";     SHA256="dbb698396d478e7fa2b1e50f4103324b2a99b90569ee27c33f2261f9215cf41e" ;;
            aarch64) ARCH="aarch64"; SHA256="69df11a02cfa3ef7d7ca645e03edce6778ec090e100f6ae2b42097865730ac52" ;;
            *) echo "unsupported architecture $(uname -m)"; exit 1 ;;
        esac
        URL="https://github.com/adoptium/temurin25-binaries/releases/download/${JDK/+/%2B}/OpenJDK25U-jdk_${ARCH}_linux_hotspot_25.0.4.1_1.tar.gz"
        tmp="$(mktemp)"
        trap 'rm -f "$tmp"' EXIT
        curl -fsSL "$URL" -o "$tmp"
        echo "$SHA256  $tmp" | sha256sum -c -
        $SUDO rm -rf /opt/jdk-25
        $SUDO mkdir -p /opt/jdk-25
        $SUDO tar -xzf "$tmp" -C /opt/jdk-25 --strip-components=1
        $SUDO ln -sf /opt/jdk-25/bin/java /usr/local/bin/java
        $SUDO ln -sf /opt/jdk-25/bin/javac /usr/local/bin/javac
        rm -f "$tmp"
        trap - EXIT
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
