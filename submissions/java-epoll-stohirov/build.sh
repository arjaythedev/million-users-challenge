#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [ -x /opt/jdk/bin/javac ]; then JAVA_HOME=/opt/jdk; fi
JDK_BIN="${JAVA_HOME:+$JAVA_HOME/bin/}"
rm -rf build/classes
mkdir -p build
"${JDK_BIN}javac" --release 25 -Xlint:all,-restricted,-serial -Werror -d build/classes $(find src -name '*.java')
printf 'Enable-Native-Access: ALL-UNNAMED\n' > build/manifest.txt
"${JDK_BIN}jar" --create --file build/server.jar --main-class challenge.Main --manifest build/manifest.txt -C build/classes .
