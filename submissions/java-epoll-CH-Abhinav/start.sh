#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

JAVA="java"
for candidate in /opt/jdk-25/bin/java /usr/lib/jvm/java-25-*/bin/java /usr/lib/jvm/*-25*/bin/java; do
    if [ -x "$candidate" ]; then
        JAVA="$candidate"
        break
    fi
done

exec "$JAVA" \
    --enable-native-access=ALL-UNNAMED \
    --add-modules jdk.incubator.vector \
    --sun-misc-unsafe-memory-access=allow \
    -XX:+UseZGC \
    -XX:-ZProactive \
    -Xms512m \
    -Xmx512m \
    -XX:+AlwaysPreTouch \
    -XX:-TieredCompilation \
    -XX:FreqInlineSize=2048 \
    -XX:MaxInlineSize=512 \
    -XX:MaxInlineLevel=18 \
    -XX:LoopUnrollLimit=120 \
    -cp build \
    challenge.Main
