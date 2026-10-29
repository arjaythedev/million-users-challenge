#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

# The compiler: FEED_COMPILER=clang (default) or gcc. Each has its own build directory, so both can sit side by side.
case "${FEED_COMPILER:-clang}" in
    clang) P=release ;;
    gcc) P=release-gcc ;;
    *) echo "build.sh: FEED_COMPILER must be clang or gcc" >&2; exit 1 ;;
esac

# Profile-guided optimisation: an instrumented build, a training run (pgo/train.cpp: a request mix through the real
# parsing, auth, JSON and gzip code, no database needed), then the real build with the profile. If any step of the
# training fails the build falls back to a plain one. FEED_PGO=off skips it.
pgo_train() {
    cmake --preset "$P" -DFEED_PGO=generate &&
        cmake --build --preset "$P" --target pgo_train &&
        "build/$P/pgo_train" &&
        # clang writes raw profiles that llvm-profdata must merge; gcc's are used as they are
        { [ "$P" = release-gcc ] || llvm-profdata-20 merge -o "build/$P/pgo/default.profdata" "build/$P"/pgo/*.profraw; }
}

if [ "${FEED_PGO:-on}" != off ]; then
    rm -rf "build/$P/pgo"
    if pgo_train; then
        cmake --preset "$P" -DFEED_PGO=use
    else
        echo "build.sh: PGO training failed, building without it" >&2
        rm -rf "build/$P/pgo"
        cmake --preset "$P" -DFEED_PGO=off
    fi
else
    cmake --preset "$P" -DFEED_PGO=off
fi

cmake --workflow --preset "$P"
