#!/usr/bin/env bash
# Cross-compile for aarch64 and run the unit tests under qemu. Dev loop only;
# the submission itself builds natively with ../build.sh.
# Usage: bash cross/build.sh [extra cmake -D options]
set -euo pipefail
cd "$(dirname "$0")/.."
docker build -q -t epoll-eastl-cross-aarch64 cross >/dev/null
docker run --rm -t \
    --user "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "$PWD:/src" -w /src \
    epoll-eastl-cross-aarch64 bash -c '
        set -euo pipefail
        cmake -S . -B build/cross-aarch64 -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/aarch64-cross.cmake -DCMAKE_BUILD_TYPE=Release "$@"
        cmake --build build/cross-aarch64
        file build/cross-aarch64/server
        ctest --test-dir build/cross-aarch64 --output-on-failure
    ' bash "$@"
