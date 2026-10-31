#!/usr/bin/env bash
# Foreground server (systemd ExecStart). Config from env: DATABASE_URL, JWT_SECRET, HOST, PORT, optional EXTRA_PORT, DB_POOL.
#   -XX:+UseG1GC            G1 explicitly: with 1 usable CPU the JVM's ergonomics would pick SerialGC.
#   -XX:MaxRAMPercentage=70 as the challenge brief prescribes; per-connection state lives on the heap.
#   MAX_RAM_PCT overrides the 70 (heap-size experiments only).
#   -XX:InitialRAMPercentage=10  start with a reasonable heap instead of growing from 1/64 of RAM.
#   -XX:+UseCompactObjectHeaders on JDK 25+: 8-byte object headers shrink per-connection heap state.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
# Linux: prefer the JDK 25 install.sh put in, else JDK 21 (a JAVA_HOME pointing at another JDK, as on CI runners, would win otherwise)
if [ "$(uname -s)" = Linux ]; then
  for jdk in /usr/lib/jvm/java-25-openjdk-* /usr/lib/jvm/java-21-openjdk-*; do
    if [ -x "$jdk/bin/java" ]; then JAVA_HOME=$jdk; break; fi
  done
fi
JAVA="${JAVA_HOME:+$JAVA_HOME/bin/}java"
# Keep G1 by default; alternative collectors are explicit benchmark overrides.
case "${GC:-g1}" in
  g1) GC_FLAGS=(-XX:+UseG1GC) ;;
  parallel) GC_FLAGS=(-XX:+UseParallelGC) ;;
  zgc) GC_FLAGS=(-XX:+UseZGC -XX:+ZGenerational) ;;
  *) printf 'Unsupported GC: %s (use g1, parallel or zgc)\n' "$GC" >&2; exit 1 ;;
esac
FEATURE=$("$JAVA" -XshowSettings:properties -version 2>&1 | sed -n 's/^ *java.specification.version = //p')
if [ "${FEATURE:-21}" -ge 25 ]; then
  GC_FLAGS+=(-XX:+UseCompactObjectHeaders)
  # Generational ZGC is the only ZGC mode from JDK 24; the flag was removed.
  FLAGS=()
  for flag in "${GC_FLAGS[@]}"; do [ "$flag" = -XX:+ZGenerational ] || FLAGS+=("$flag"); done
  GC_FLAGS=("${FLAGS[@]}")
fi
exec "$JAVA" -server "${GC_FLAGS[@]}" -XX:MaxRAMPercentage=${MAX_RAM_PCT:-70} -XX:InitialRAMPercentage=10 \
  -jar "$DIR/target/quarkus-app/quarkus-run.jar"
