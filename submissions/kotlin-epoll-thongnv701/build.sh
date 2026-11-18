#!/usr/bin/env bash
# Builds build/server.jar (the server plus the Kotlin stdlib) from src/ with kotlinc. Runs as a normal user.
set -euo pipefail
cd "$(dirname "$0")"
if [ -x /opt/jdk/bin/java ]; then export JAVA_HOME=/opt/jdk; fi
: "${JAVA_HOME:?install.sh puts the JDK in /opt/jdk, or set JAVA_HOME}"
KOTLINC=kotlinc
if [ -x /opt/kotlinc/bin/kotlinc ]; then KOTLINC=/opt/kotlinc/bin/kotlinc; fi

rm -rf build
mkdir -p build
"$KOTLINC" -jdk-home "$JAVA_HOME" -jvm-target 25 -include-runtime -Werror \
  -Xno-param-assertions -Xno-call-assertions -Xno-receiver-assertions \
  -d build/server.jar src/challenge/*.kt

# Every FFM downcall must have an exact primitive call-site type. A boxed or Object argument would throw
# WrongMethodTypeException at run time, so fail the build instead.
classes="$("$JAVA_HOME/bin/jar" tf build/server.jar | grep '^challenge/.*\.class$' | sed 's/\.class$//; s#/#.#g')"
# shellcheck disable=SC2086
bad="$("$JAVA_HOME/bin/javap" -c -p -cp build/server.jar $classes \
  | grep -E 'MethodHandle\.invokeExact:\(' \
  | grep -E 'Ljava/lang/(Object|Integer|Long|Short|Byte|Boolean);|\[Ljava/lang/Object;' || true)"
if [ -n "$bad" ]; then
  echo "build.sh: an FFM call site is not exact:" >&2
  echo "$bad" >&2
  exit 1
fi
echo "built build/server.jar"
