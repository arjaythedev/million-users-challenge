#!/usr/bin/env bash
# Foreground server (systemd ExecStart). Config from env: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT; optional
# THREADS, DB_READ_CONNS, DB_WRITE_CONNS, IO, HEAP.
#   --enable-native-access  the epoll backend calls libc through the Foreign Function & Memory API
#   -XX:+UseParallelGC      requests allocate almost nothing, so collections are rare and short
#   -Xmx${HEAP:-4g}         the heap holds per-connection arrays (~50 bytes each) and small buffers, not connections
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
# Linux: run on the JDK 25 install.sh put in (a JAVA_HOME pointing at another JDK, as on CI runners, would win otherwise)
if [ -d /usr/lib/jvm/java-25-openjdk-amd64 ] && [ "$(uname -s)" = Linux ]; then JAVA_HOME=/usr/lib/jvm/java-25-openjdk-amd64; fi
if [ -z "${JAVA_HOME:-}" ] && [ "$(uname -s)" = Darwin ]; then JAVA_HOME="$(/usr/libexec/java_home -v 25 2>/dev/null || true)"; fi
JAVA="${JAVA_HOME:+$JAVA_HOME/bin/}java"
exec "$JAVA" --enable-native-access=ALL-UNNAMED -XX:+UseParallelGC -Xms1g -Xmx"${HEAP:-4g}" -jar "$DIR/target/feed.jar"
