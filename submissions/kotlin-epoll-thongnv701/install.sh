#!/usr/bin/env bash
# Toolchain: Eclipse Temurin 25 (official tarball) and the Kotlin 2.4.21 compiler (official zip), both
# sha256-pinned. Idempotent; run as root on Ubuntu 24.04 (x86_64 is the benchmark box, aarch64 is for local runs).
set -euo pipefail
JDK=jdk-25.0.4.1+1
KOTLIN=2.4.21
KOTLIN_SHA256=7cc140e76daf416a0424a5557f99afd7ca89ebd463ea1101261e3e01c33e75cd

have_jdk() { [ -x /opt/jdk/bin/javac ] && /opt/jdk/bin/java -version 2>&1 | grep -q '"25.0.4.1"'; }
have_kotlin() { [ "$(cat /opt/kotlinc/build.txt 2>/dev/null)" = "$KOTLIN" ]; }
if have_jdk && have_kotlin; then echo "Temurin $JDK and Kotlin $KOTLIN already installed"; exit 0; fi

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends curl ca-certificates unzip >/dev/null
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

if ! have_jdk; then
  case "$(uname -m)" in
    x86_64)  ARCH=x64;     SHA256=dbb698396d478e7fa2b1e50f4103324b2a99b90569ee27c33f2261f9215cf41e ;;
    aarch64) ARCH=aarch64; SHA256=69df11a02cfa3ef7d7ca645e03edce6778ec090e100f6ae2b42097865730ac52 ;;
    *) echo "unsupported architecture $(uname -m)"; exit 1 ;;
  esac
  URL="https://github.com/adoptium/temurin25-binaries/releases/download/${JDK/+/%2B}/OpenJDK25U-jdk_${ARCH}_linux_hotspot_25.0.4.1_1.tar.gz"
  curl -fsSL --retry 3 "$URL" -o "$tmp/jdk.tar.gz"
  echo "$SHA256  $tmp/jdk.tar.gz" | sha256sum -c -
  rm -rf /opt/jdk && mkdir -p /opt/jdk
  tar -xzf "$tmp/jdk.tar.gz" -C /opt/jdk --strip-components=1
fi

if ! have_kotlin; then
  curl -fsSL --retry 3 "https://github.com/JetBrains/kotlin/releases/download/v$KOTLIN/kotlin-compiler-$KOTLIN.zip" \
    -o "$tmp/kotlinc.zip"
  echo "$KOTLIN_SHA256  $tmp/kotlinc.zip" | sha256sum -c -
  rm -rf /opt/kotlinc
  unzip -q "$tmp/kotlinc.zip" -d "$tmp"
  mv "$tmp/kotlinc" /opt/kotlinc
fi

/opt/jdk/bin/java -version
JAVA_HOME=/opt/jdk /opt/kotlinc/bin/kotlinc -version
