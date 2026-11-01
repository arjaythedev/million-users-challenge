#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04 (idempotent). Installs the build-time toolchain only; the server is a
# native binary with no JVM at run time:
#   - OpenJDK 21 from Ubuntu's own apt repository (signed packages, version fixed by the 24.04 release), to run
#     Gradle and the Kotlin/Native compiler
#   - Gradle 9.8.2, the official distribution, checked against its pinned SHA-256
# build.sh then lets Gradle download the pinned Kotlin 2.4.21 / Kotlin/Native toolchain (compiler + LLVM + sysroot).
set -euo pipefail

JDK_PKG=openjdk-21-jdk-headless
JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64
GRADLE_VERSION=9.8.2
GRADLE_SHA256=21bd5da2de49b642904077f38c0efdc7778e92b3ed0889392dca61b97e389607
GRADLE_HOME="/opt/gradle/gradle-${GRADLE_VERSION}"

if ! dpkg -s "$JDK_PKG" >/dev/null 2>&1 || ! command -v curl >/dev/null || ! command -v unzip >/dev/null; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -q
    apt-get install -y -q --no-install-recommends "$JDK_PKG" curl ca-certificates unzip
fi
"$JAVA_HOME/bin/java" -version

if [ ! -x "$GRADLE_HOME/bin/gradle" ]; then
    tmp="$(mktemp -d)"
    curl -fsSLo "$tmp/gradle.zip" "https://services.gradle.org/distributions/gradle-${GRADLE_VERSION}-bin.zip"
    echo "$GRADLE_SHA256  $tmp/gradle.zip" | sha256sum -c -
    mkdir -p /opt/gradle
    unzip -q "$tmp/gradle.zip" -d /opt/gradle
    rm -rf "$tmp"
fi
ln -sfn "$GRADLE_HOME/bin/gradle" /usr/local/bin/gradle
JAVA_HOME="$JAVA_HOME" gradle --version
