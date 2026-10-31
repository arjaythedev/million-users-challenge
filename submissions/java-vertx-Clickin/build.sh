#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if javac=$(command -v javac); then
  javac=$(readlink -f "$javac")
  export JAVA_HOME="${javac%/bin/javac}"
fi
./mvnw -q -DskipTests clean package
