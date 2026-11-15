#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

if [ -d /usr/lib/jvm/java-21-openjdk-amd64 ] && [ "$(uname -s)" = Linux ]; then
  export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64
fi

bash ./mvnw -B -q -DskipTests package
