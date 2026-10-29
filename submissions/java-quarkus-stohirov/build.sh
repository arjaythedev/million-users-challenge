#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [ -z "${JAVA_HOME:-}" ] && [ "$(uname -s)" = Darwin ]; then
  if JH="$(/usr/libexec/java_home -v 21 2>/dev/null)"; then export JAVA_HOME="$JH"; fi
fi
if [ -d /usr/lib/jvm/java-21-openjdk-amd64 ] && [ "$(uname -s)" = Linux ]; then export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64; fi
./mvnw -B -q -DskipTests package
ls -l target/quarkus-app/quarkus-run.jar
