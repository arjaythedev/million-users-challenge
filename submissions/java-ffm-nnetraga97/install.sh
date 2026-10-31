#!/usr/bin/env bash
# Ubuntu 24.04, as root. Idempotent: installs OpenJDK 25 (JDK, needed to build) and libdeflate (gzip) if missing.
# Maven comes from the checked-in wrapper (./mvnw). epoll is in libc; without libdeflate, gzip falls back to the JDK.
set -euo pipefail
if java -version 2>&1 | grep -q 'version "25' && command -v javac >/dev/null; then
  echo "OpenJDK 25 already installed"
else
  apt-get update
  apt-get install -y --no-install-recommends openjdk-25-jdk-headless
fi
if ! ldconfig -p | grep 'libdeflate\.so\.0' >/dev/null; then  # no -q: under pipefail an early exit fails the pipe
  apt-get install -y --no-install-recommends libdeflate0 || { apt-get update && apt-get install -y --no-install-recommends libdeflate0; }
fi
java -version
