#!/usr/bin/env bash
set -euo pipefail
if java -version 2>&1 | grep -q 'version "21' && command -v javac >/dev/null; then
  echo "OpenJDK 21 already installed"
else
  apt-get update
  apt-get install -y --no-install-recommends openjdk-21-jdk-headless
fi
java -version
