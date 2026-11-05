#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT}/build"

C_COMPILER="${CC:-$(command -v gcc-14 || command -v gcc || true)}"
CXX_COMPILER="${CXX:-$(command -v g++-14 || command -v g++ || true)}"

if [[ -z "${C_COMPILER}" || -z "${CXX_COMPILER}" ]]; then
  echo "Error: GCC and G++ are required." >&2
  exit 1
fi

command -v cmake >/dev/null || {
  echo "Error: CMake is required." >&2
  exit 1
}

echo "Using C compiler:   ${C_COMPILER}"
echo "Using C++ compiler: ${CXX_COMPILER}"

cmake -S "${ROOT}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="${C_COMPILER}" \
  -DCMAKE_CXX_COMPILER="${CXX_COMPILER}" \
  -DHTTPSERVER_NATIVE_ARCH=ON \
  -DHTTPSERVER_FULL_STATIC=OFF

cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

if [[ ! -x "${BUILD_DIR}/.http_server" ]]; then
  echo "Build failed: executable not found." >&2
  exit 1
fi

echo "Build successful: ${BUILD_DIR}/.http_server"
