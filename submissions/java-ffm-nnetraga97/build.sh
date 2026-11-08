#!/usr/bin/env bash
# Build target/feed.jar with the checked-in Maven wrapper. Idempotent. Needs JDK 25.
set -euo pipefail
cd "$(dirname "$0")"
if [ -z "${JAVA_HOME:-}" ] && [ "$(uname -s)" = Darwin ]; then
  if JH="$(/usr/libexec/java_home -v 25 2>/dev/null)"; then export JAVA_HOME="$JH"; fi
fi
# Linux: build with the JDK 25 install.sh put in (a JAVA_HOME pointing at another JDK, as on CI runners, would win otherwise)
if [ -d /usr/lib/jvm/java-25-openjdk-amd64 ] && [ "$(uname -s)" = Linux ]; then export JAVA_HOME=/usr/lib/jvm/java-25-openjdk-amd64; fi
./mvnw -B -q -DskipTests package
ls -l target/feed.jar
