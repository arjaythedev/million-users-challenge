#!/usr/bin/env bash
set -Eeuo pipefail

# ============================================================
# Configuration
# ============================================================

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly BUILD_DIR="${SCRIPT_DIR}/build"
readonly SERVER="${BUILD_DIR}/http_server"

readonly C_COMPILER="${CC:-gcc-14}"
readonly CXX_COMPILER="${CXX:-g++-14}"

readonly BUILD_TYPE="Release"
readonly BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"

# ============================================================
# Helpers
# ============================================================

error() {
  echo "ERROR: $*" >&2
  exit 1
}

require_command() {
  command -v "$1" >/dev/null 2>&1 ||
    error "Required command not found: $1"
}

# ============================================================
# Validate toolchain
# ============================================================

echo "============================================"
echo " Building HttpServer"
echo "============================================"

require_command "${C_COMPILER}"
require_command "${CXX_COMPILER}"
require_command cmake
require_command ninja
require_command pkg-config
require_command file
require_command ldd

echo
echo "=== Toolchain ==="

"${C_COMPILER}" --version | head -n 1
"${CXX_COMPILER}" --version | head -n 1
cmake --version | head -n 1

# ============================================================
# Validate dependencies
# ============================================================

echo
echo "=== Dependencies ==="

for module in libpq liburing openssl libdeflate; do
  if ! pkg-config --exists "${module}"; then
    error "Missing pkg-config dependency: ${module}. Run ./install.sh first."
  fi

  printf "%-12s %s\n" \
    "${module}:" \
    "$(pkg-config --modversion "${module}")"
done

LIBPQ_VERSION="$(pkg-config --modversion libpq)"

if [[ "${LIBPQ_VERSION%%.*}" != "18" ]]; then
  error "Expected PostgreSQL/libpq 18, found ${LIBPQ_VERSION}."
fi

# ============================================================
# Configure
# ============================================================

echo
echo "=== CMake configuration ==="

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DCMAKE_C_COMPILER="${C_COMPILER}" \
  -DCMAKE_CXX_COMPILER="${CXX_COMPILER}" \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
  -DHTTPSERVER_NATIVE_ARCH=ON \
  -DHTTPSERVER_FULL_STATIC=ON

# ============================================================
# Build
# ============================================================

echo
echo "=== Compilation ==="

cmake --build "${BUILD_DIR}" \
  --parallel "${BUILD_JOBS}" \
  --verbose

[[ -x "${SERVER}" ]] ||
  error "Build did not produce executable: ${SERVER}"

# ============================================================
# Verify executable
# ============================================================

echo
echo "=== Executable ==="

file "${SERVER}"

echo
echo "=== Dynamic dependency verification ==="

LDD_OUTPUT="$(ldd "${SERVER}" 2>&1 || true)"

if grep -qE 'not a dynamic executable|statically linked' <<<"${LDD_OUTPUT}"; then
  echo "Verified: executable has no dynamic shared-library dependencies."
else
  echo "${LDD_OUTPUT}"
  error "Executable is dynamically linked. Check HTTPSERVER_FULL_STATIC and CMake link configuration."
fi

# ============================================================
# Final information
# ============================================================

echo
echo "=== Binary size ==="

du -h "${SERVER}"

echo
echo "============================================"
echo " Build completed successfully."
echo " Executable: ${SERVER}"
echo " Build type: ${BUILD_TYPE}"
echo " LTO:        ON"
echo " Native arch: ON"
echo " Static:     VERIFIED"
echo "============================================"
