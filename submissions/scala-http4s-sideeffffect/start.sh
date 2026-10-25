#!/usr/bin/env bash
# Runs the server in the foreground. Config: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT.
# Optional: DB_READ_SESSIONS (default 128), DB_WRITE_SESSIONS (default 32), COMPUTE_THREADS (default: all cores),
# MAX_CONNECTIONS (per port, default 1,000,000), HEAP_PERCENT (default 75).
set -euo pipefail
cd "$(dirname "$0")"
if [ -x /opt/jdk-27/bin/java ]; then JAVA=/opt/jdk-27/bin/java
else JAVA="${JAVA_HOME:?no JDK in /opt/jdk-27 (run install.sh) and JAVA_HOME is not set}/bin/java"; fi
# Generational ZGC (the only ZGC mode in JDK 27): a million connections are a large live set that G1 and Parallel
#   would have to copy or mark in pauses; ZGC does it concurrently, so pauses stay under a millisecond (README.md).
# Heap: 75% of RAM (Postgres is on another machine). Fixed and pre-touched: there is no swap, and a heap that grows
#   later can fail when the kernel's socket buffers have taken the memory.
# Fiber tracing is off: it records a stack frame per IO step, which costs CPU on every request.
exec "$JAVA" \
  -XX:+UseZGC -XX:MaxRAMPercentage="${HEAP_PERCENT:-75}" -XX:InitialRAMPercentage="${HEAP_PERCENT:-75}" \
  -XX:+AlwaysPreTouch -XX:-UsePerfData \
  -Dcats.effect.tracing.mode=none --sun-misc-unsafe-memory-access=allow --enable-native-access=ALL-UNNAMED \
  -cp "$(cat target/classpath.txt)" feed.Main
