#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
rm -rf build
mkdir -p build

JAVAC="javac"
for candidate in /opt/jdk-25/bin/javac /usr/lib/jvm/java-25-*/bin/javac /usr/lib/jvm/*-25*/bin/javac; do
    if [ -x "$candidate" ]; then
        JAVAC="$candidate"
        break
    fi
done

"$JAVAC" --release 25 \
      --add-modules jdk.incubator.vector \
      -XDignore.symbol.file \
      -d build \
      src/challenge/*.java
