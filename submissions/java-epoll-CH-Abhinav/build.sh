#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
rm -rf build
mkdir -p build

JAVAC="javac"
if [ -x /opt/jdk-25/bin/javac ]; then
    JAVAC="/opt/jdk-25/bin/javac"
fi

"$JAVAC" --release 25 \
      --add-modules jdk.incubator.vector \
      -XDignore.symbol.file \
      -d build \
      src/challenge/*.java
