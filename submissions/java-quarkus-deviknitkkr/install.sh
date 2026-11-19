#!/usr/bin/env bash
# Ubuntu 24.04, as root. Idempotent: installs OpenJDK 25 (JDK, needed to build) only if missing.
# Maven comes from the checked-in wrapper (./mvnw). Bytecode targets Java 21, so JDK 21 still works.
set -euo pipefail
if ls /usr/lib/jvm/java-25-openjdk-*/bin/javac >/dev/null 2>&1; then
  echo "OpenJDK 25 already installed"
else
  apt-get update
  apt-get install -y --no-install-recommends openjdk-25-jdk-headless
fi
/usr/lib/jvm/java-25-openjdk-*/bin/java -version
