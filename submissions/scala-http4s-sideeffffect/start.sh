#!/usr/bin/env bash
# Runs the server in the foreground. Config: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT.
# Optional: DB_READ_SESSIONS (default 128), DB_WRITE_SESSIONS (default 32), COMPUTE_THREADS (default: all cores),
# MAX_CONNECTIONS (per port, default 1,000,000), HEAP_PERCENT (default 75).
set -euo pipefail
cd "$(dirname "$0")"
if [ -x /opt/jdk-27/bin/java ]; then JAVA_HOME=/opt/jdk-27; fi
: "${JAVA_HOME:?no JDK in /opt/jdk-27 (run install.sh) and JAVA_HOME is not set}"
# Generational ZGC (the only ZGC mode in JDK 27): a million connections are a large live set that G1 and Parallel
#   would have to copy or mark in pauses; ZGC does it concurrently, so pauses stay under a millisecond (README.md).
# Heap: 75% of RAM (Postgres is on another machine). Fixed and pre-touched: there is no swap, and a heap that grows
#   later can fail when the kernel's socket buffers have taken the memory.
# Fiber tracing is off: it records a stack frame per IO step, which costs CPU on every request.
SCALA_CLI=/opt/scala-cli/scala-cli; [ -x "$SCALA_CLI" ] || SCALA_CLI=scala-cli
# scala-cli runs the classes build.sh compiled (target/classpath.txt) without compiling them again: --java because
# the Scala library is already on that class path. --offline (needs --power): nothing is fetched, only build.sh
# downloads. --scala: the version project.scala uses, which build.sh fetched (scala-cli loads that compiler even with
# nothing to compile, and its default version isn't in the cache). It replaces itself with the JVM, so the server keeps
# its PID.
exec "$SCALA_CLI" --power run --server=false --offline --scala 3.10.0 --java-home "$JAVA_HOME" --java -cp "$(cat target/classpath.txt)" \
  --main-class feed.Main \
  --java-opt=-XX:+UseZGC --java-opt=-XX:MaxRAMPercentage="${HEAP_PERCENT:-75}" --java-opt=-XX:InitialRAMPercentage="${HEAP_PERCENT:-75}" \
  --java-opt=-XX:+AlwaysPreTouch --java-opt=-XX:-UsePerfData --java-opt=-Dcats.effect.tracing.mode=none \
  --java-opt=--sun-misc-unsafe-memory-access=allow --java-opt=--enable-native-access=ALL-UNNAMED
