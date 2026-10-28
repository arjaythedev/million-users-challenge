#!/usr/bin/env bash
# Foreground server (systemd ExecStart). Config from env: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT.
# Optional: LOOPS (event loops, default: the cores the JVM sees), DB_POOL (read connections in all, default 64),
# GZIP_LEVEL (0 turns gzip off), GZIP_MIN (smallest body to compress, default 1024), HEAP (default 2g).
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -d /opt/jdk27 ]; then JAVA_HOME=/opt/jdk27; fi
exec "${JAVA_HOME:+$JAVA_HOME/bin/}java" --enable-native-access=ALL-UNNAMED -XX:+UseParallelGC \
  -Xms"${HEAP:-2g}" -Xmx"${HEAP:-2g}" -XX:+AlwaysPreTouch -XX:-UsePerfData -cp "$DIR/build/server.jar" challenge.Main
