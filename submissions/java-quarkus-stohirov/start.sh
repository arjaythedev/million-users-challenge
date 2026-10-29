#!/usr/bin/env bash
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -d /usr/lib/jvm/java-21-openjdk-amd64 ] && [ "$(uname -s)" = Linux ]; then JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64; fi
JAVA="${JAVA_HOME:+$JAVA_HOME/bin/}java"
IO_THREADS="${IO_THREADS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}"
exec "$JAVA" -server -XX:+UseG1GC -XX:MaxRAMPercentage=${MAX_RAM_PCT:-50} -XX:InitialRAMPercentage=10 \
  -XX:MaxDirectMemorySize=${MAX_DIRECT:-6g} \
  -Dquarkus.vertx.event-loops-pool-size="$IO_THREADS" -Dquarkus.http.io-threads="$IO_THREADS" \
  -jar "$DIR/target/quarkus-app/quarkus-run.jar"
