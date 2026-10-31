#!/usr/bin/env bash
# Compiles src/ into build/server.jar with the JDK alone. Idempotent.
set -euo pipefail
cd "$(dirname "$0")"
if [ -d /opt/jdk27 ]; then export JAVA_HOME=/opt/jdk27; fi
JAVA_BIN="${JAVA_HOME:+$JAVA_HOME/bin/}"
"${JAVA_BIN}java" -version 2>&1 | grep -q 'version "2[5-9]' || { echo "JDK 25+ required (run install.sh, or set JAVA_HOME)" >&2; exit 1; }
rm -rf build && mkdir -p build/classes
"${JAVA_BIN}javac" --release 25 -Xlint:all -d build/classes src/challenge/*.java
"${JAVA_BIN}jar" --create --file build/server.jar --main-class challenge.Main -C build/classes .
ls -l build/server.jar
