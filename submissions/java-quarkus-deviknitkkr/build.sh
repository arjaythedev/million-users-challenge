#!/usr/bin/env bash
# Build the Quarkus fast-jar with the checked-in Maven wrapper. Idempotent.
set -euo pipefail
cd "$(dirname "$0")"
if [ -z "${JAVA_HOME:-}" ] && [ "$(uname -s)" = Darwin ]; then
  if JH="$(/usr/libexec/java_home -v 21 2>/dev/null)"; then export JAVA_HOME="$JH"; fi
fi
# Linux: build with the JDK install.sh put in (a JAVA_HOME pointing at another JDK, as on CI runners, would win otherwise)
if [ "$(uname -s)" = Linux ]; then
  for jdk in /usr/lib/jvm/java-25-openjdk-* /usr/lib/jvm/java-21-openjdk-*; do
    if [ -x "$jdk/bin/javac" ]; then export JAVA_HOME=$jdk; break; fi
  done
fi
./mvnw -B -q -DskipTests clean package
ls -l target/quarkus-app/quarkus-run.jar
