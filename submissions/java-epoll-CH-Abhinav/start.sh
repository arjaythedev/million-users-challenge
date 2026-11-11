#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

JAVA="java"
if [ -x /opt/jdk-25/bin/java ]; then
    JAVA="/opt/jdk-25/bin/java"
fi

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
