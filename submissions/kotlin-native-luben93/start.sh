#!/usr/bin/env bash
# Runs the server in the foreground: a native binary, no JVM. Config: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT.
# Optional process knobs (defaults sized from the CPUs we may run on): HTTP_THREADS, DB_READ_CONNS, DB_WRITE_CONNS,
# GZIP_LEVEL, GZIP_MIN_BYTES, STATS_INTERVAL_S. JVM=1 runs the JVM variant instead (lab A/B only, build with JVM=1).
set -euo pipefail
cd "$(dirname "$0")"
# one fd per client connection: soft limit = hard limit (a no-op under systemd's LimitNOFILE, needed from a shell)
ulimit -n "$(ulimit -Hn)"
if [ "${JVM:-0}" = 1 ]; then
    exec "${BUILD_JAVA_HOME:-/usr/lib/jvm/java-21-openjdk-amd64}/bin/java" -XX:+UseTransparentHugePages -XX:+UseSystemMemoryBarrier -XX:-UsePerfData \
        -cp "build/jvm/feed.jar:build/jvm/lib/*" feed.MainKt
fi
exec build/bin/linuxX64/releaseExecutable/feed.kexe
