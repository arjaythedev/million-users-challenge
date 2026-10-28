#!/usr/bin/env bash
# Builds the Kotlin/Native server, build/bin/linuxX64/releaseExecutable/feed.kexe, as a normal user. Idempotent.
# Gradle (from install.sh) runs on its JDK and downloads the pinned Kotlin/Native toolchain (compiler + LLVM) itself;
# the first build takes several minutes.
#
# Dev overrides (not used on the box): JAVA_HOME (default /opt/jdk), GRADLE (default gradle), GRADLE_USER_HOME and
# KONAN_DATA_DIR (dependency caches, e.g. the ones .tmp/scripts/fetch-deps.sh fills), OFFLINE=1 (gradle --offline),
# JVM=1 (also build the JVM variant into build/jvm).
#
# Codegen targets this machine's CPU (the box builds what it runs): Kotlin/Native's clang reports the CPU name and
# its full +/- feature list for -march=native, passed to the compiler via build.gradle.kts. If that fails, the
# binary is built for Kotlin/Native's portable x86-64 default.
set -euo pipefail
cd "$(dirname "$0")"
export JAVA_HOME="${JAVA_HOME:-/opt/jdk}"
if [ ! -x "$JAVA_HOME/bin/java" ]; then
    echo "build.sh: no JDK at $JAVA_HOME; run install.sh first" >&2
    exit 1
fi
export PATH="$JAVA_HOME/bin:$PATH"
GRADLE="${GRADLE:-gradle}"
gradle_args=(--no-daemon -q)
[ "${OFFLINE:-0}" = 1 ] && gradle_args+=(--offline)

# compiling the main sources makes Gradle fetch the Kotlin/Native toolchain (its clang included) on a first build
"$GRADLE" "${gradle_args[@]}" compileKotlinLinuxX64

cpu_args=()
detect_cpu() {
    local clang out cpu feat
    for clang in "${KONAN_DATA_DIR:-$HOME/.konan}"/dependencies/llvm-*-essentials-*/bin/clang; do
        [ -x "$clang" ] || continue
        out=$("$clang" -march=native -### -c -x c /dev/null -o /dev/null 2>&1) || continue
        cpu=$(printf '%s' "$out" | grep -o '"-target-cpu" "[^"]*"' | head -1 | cut -d'"' -f4)
        feat=$(printf '%s' "$out" | grep -o '"-target-feature" "[^"]*"' | cut -d'"' -f4 | paste -sd, -)
        if [ -n "$cpu" ] && [ -n "$feat" ] && [ "$cpu" != x86-64 ]; then
            echo "build.sh: codegen for this CPU: $cpu, $(tr ',' '\n' <<<"$feat" | grep -c '^+') features on (via $clang)"
            cpu_args=("-Pfeed.targetCpu=$cpu" "-Pfeed.targetCpuFeatures=$feat")
            return 0
        fi
    done
    return 1
}
if [ "${FEED_GENERIC_CPU:-0}" = 1 ] || ! detect_cpu; then
    echo "build.sh: codegen for generic x86-64 (CPU detection off or failed)"
fi

tasks=(linkReleaseExecutableLinuxX64)
[ "${JVM:-0}" = 1 ] && tasks+=(installJvm)
"$GRADLE" "${gradle_args[@]}" "${tasks[@]}" "${cpu_args[@]}"
ls -l build/bin/linuxX64/releaseExecutable/feed.kexe
