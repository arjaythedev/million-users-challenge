#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [ -x /opt/jdk/bin/java ]; then JAVA_HOME=/opt/jdk; fi
exec "${JAVA_HOME:+$JAVA_HOME/bin/}java" \
  -XX:+UseParallelGC -Xms2g -Xmx2g -XX:+AlwaysPreTouch -XX:+UseCompactObjectHeaders \
  -XX:MaxDirectMemorySize=256m -XX:ReservedCodeCacheSize=64m -XX:MaxMetaspaceSize=64m \
  -XX:CompileCommand=quiet -XX:CompileCommand=dontinline,challenge.Gzip::tokenize \
  -XX:CompileCommand=dontinline,challenge.Gzip::data \
  -Dsun.net.inetaddr.negative.ttl=0 --enable-native-access=ALL-UNNAMED \
  -jar build/server.jar
