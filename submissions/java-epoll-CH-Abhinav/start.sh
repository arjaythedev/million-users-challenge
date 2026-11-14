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
    -XX:+UseZGC \
    -XX:-ZProactive \
    -Xms512m \
    -Xmx512m \
    -XX:+AlwaysPreTouch \
    -XX:-TieredCompilation \
    -cp build \
    challenge.Main
