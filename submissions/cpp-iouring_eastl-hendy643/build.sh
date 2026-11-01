#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

P=release

pgo_train() {
    cmake --preset "$P" -DFEED_PGO=generate &&
        cmake --build --preset "$P" --target pgo_train &&
        "build/$P/pgo_train"
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
