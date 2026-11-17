#!/usr/bin/env bash
set -Eeuo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "ERROR: install.sh must be run as root." >&2
  exit 1
fi

export DEBIAN_FRONTEND=noninteractive

# ============================================================
# Configuration
# ============================================================

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly BUILD_DIR="${SCRIPT_DIR}/build"

readonly PG_MAJOR=18
readonly PG_VERSION=18.7

readonly MULTIARCH="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"
readonly LIB_DIR="/usr/lib/${MULTIARCH}"

readonly PG_STATIC_PREFIX="/opt/httpserver-static/postgresql-${PG_MAJOR}"

readonly PG_SOURCE_ROOT="/usr/local/src/httpserver-dependencies"
readonly PG_SOURCE_DIR="${PG_SOURCE_ROOT}/postgresql-${PG_VERSION}"
readonly PG_BUILD_DIR="${PG_SOURCE_ROOT}/postgresql-${PG_VERSION}-build"

readonly PG_TARBALL="${PG_SOURCE_ROOT}/postgresql-${PG_VERSION}.tar.bz2"
readonly PG_SHA256="${PG_TARBALL}.sha256"

readonly PG_SOURCE_URL="https://ftp.postgresql.org/pub/source/v${PG_VERSION}/postgresql-${PG_VERSION}.tar.bz2"

readonly JOBS="$(nproc)"

# ============================================================
# Helpers
# ============================================================

log() {
  printf '\n\033[1;34m==> %s\033[0m\n' "$*"
}

fail() {
  echo "ERROR: $*" >&2
  exit 1
}

require_file() {
  local path="$1"

  [[ -s "${path}" ]] ||
    fail "Required file is missing or empty: ${path}"

  printf 'OK  %s\n' "${path}"
}

require_archive() {
  local archive="$1"
  local directory="$2"

  require_file "${directory}/${archive}"
}

require_symbol() {
  local archive="$1"
  local symbol="$2"
  local symbols

  symbols="$(nm -g --defined-only "${archive}" 2>/dev/null | awk '{print $NF}')" ||
    fail "Could not inspect symbols in ${archive}"

  if ! grep -Fxq "${symbol}" <<<"${symbols}"; then
    fail "Missing symbol '${symbol}' in ${archive}"
  fi

  printf 'OK  %s: %s\n' "${archive}" "${symbol}"
}

# ============================================================
# Operating system
# ============================================================

log "Checking operating system"

. /etc/os-release

[[ "${ID}" == "ubuntu" ]] ||
  fail "This installer supports Ubuntu only."

[[ "${VERSION_ID}" == "24.04" ]] ||
  fail "Expected Ubuntu 24.04, found ${VERSION_ID}."

echo "Ubuntu:      ${VERSION_ID}"
echo "Codename:    ${VERSION_CODENAME}"
echo "Multiarch:   ${MULTIARCH}"
echo "CPU jobs:    ${JOBS}"

# ============================================================
# Base dependencies and PostgreSQL source-build tools
# ============================================================

log "Installing build dependencies"

apt-get update -qq

apt-get install -y --no-install-recommends \
  ca-certificates \
  curl \
  bzip2 \
  build-essential \
  gcc-14 \
  g++-14 \
  libgcc-14-dev \
  libstdc++-14-dev \
  libc6-dev \
  cmake \
  ninja-build \
  pkg-config \
  bison \
  flex \
  gawk \
  perl \
  libssl-dev \
  zlib1g-dev \
  liburing-dev \
  libdeflate-dev

# ============================================================
# Verify build tools
# ============================================================

log "Checking compiler and PostgreSQL build tools"

gcc-14 --version | head -n 1
g++-14 --version | head -n 1
cmake --version | head -n 1
ninja --version
make --version | head -n 1
bison --version | head -n 1
flex --version
perl --version | head -n 2

for tool in gcc-14 g++-14 cmake ninja make bison flex gawk perl; do
  command -v "${tool}" >/dev/null 2>&1 ||
    fail "Missing build tool: ${tool}"
done

# ============================================================
# Verify mandatory system static archives
# ============================================================

log "Checking system static archives"

require_archive libssl.a "${LIB_DIR}"
require_archive libcrypto.a "${LIB_DIR}"
require_archive libz.a "${LIB_DIR}"
require_archive liburing.a "${LIB_DIR}"
require_archive libdeflate.a "${LIB_DIR}"
require_archive libc.a "${LIB_DIR}"
require_archive libm.a "${LIB_DIR}"

# GCC runtime archives.
for archive in libgcc.a libstdc++.a; do
  case "${archive}" in
  libgcc.a)
    path="$(gcc-14 -print-file-name="${archive}")"
    ;;
  libstdc++.a)
    path="$(g++-14 -print-file-name="${archive}")"
    ;;
  esac

  require_file "${path}"
done

# ============================================================
# Obtain official PostgreSQL source
# ============================================================

log "Downloading PostgreSQL ${PG_VERSION} source"

install -d -m 0755 "${PG_SOURCE_ROOT}"

curl \
  --fail \
  --silent \
  --show-error \
  --location \
  --retry 3 \
  "${PG_SOURCE_URL}" \
  -o "${PG_TARBALL}"

curl \
  --fail \
  --silent \
  --show-error \
  --location \
  --retry 3 \
  "${PG_SOURCE_URL}.sha256" \
  -o "${PG_SHA256}"

EXPECTED_SHA256="$(
  grep -Eo '[[:xdigit:]]{64}' "${PG_SHA256}" | head -n 1
)"

[[ -n "${EXPECTED_SHA256}" ]] ||
  fail "Could not extract PostgreSQL source SHA256."

printf '%s  %s\n' \
  "${EXPECTED_SHA256}" \
  "${PG_TARBALL}" |
  sha256sum --check -

# ============================================================
# Extract source
# ============================================================

log "Extracting PostgreSQL source"

rm -rf -- "${PG_SOURCE_DIR}" "${PG_BUILD_DIR}"

tar -xjf "${PG_TARBALL}" -C "${PG_SOURCE_ROOT}"

[[ -d "${PG_SOURCE_DIR}" ]] ||
  fail "PostgreSQL source extraction failed."

install -d -m 0755 "${PG_BUILD_DIR}"

# ============================================================
# Configure PostgreSQL static client
# ============================================================

log "Configuring PostgreSQL static client"

(
  cd "${PG_BUILD_DIR}"

  CC=gcc-14 \
    CXX=g++-14 \
    CFLAGS="-O2 -pipe" \
    "${PG_SOURCE_DIR}/configure" \
    --prefix="${PG_STATIC_PREFIX}" \
    --with-ssl=openssl \
    --with-zlib \
    --without-gssapi \
    --without-ldap \
    --without-libcurl \
    --without-readline \
    --without-icu \
    --without-lz4 \
    --without-zstd \
    --disable-rpath
)

# ============================================================
# Build PostgreSQL internal static libraries
# ============================================================

log "Building PostgreSQL libpgport variants"

make \
  -C "${PG_BUILD_DIR}/src/port" \
  --jobs="${JOBS}" \
  all

log "Building PostgreSQL libpgcommon variants"

make \
  -C "${PG_BUILD_DIR}/src/common" \
  --jobs="${JOBS}" \
  all

# ============================================================
# Build libpq
# ============================================================

log "Building PostgreSQL libpq"

make \
  -C "${PG_BUILD_DIR}/src/interfaces/libpq" \
  --jobs="${JOBS}" \
  all

# IMPORTANT:
# libpq.a must be paired with the _shlib frontend variants.
#
# libpgcommon.a uses USE_PRIVATE_ENCODING_FUNCS.
# libpgcommon_shlib.a exports the public encoding functions
# referenced by libpq.a.

readonly PG_LIBPQ_ARCHIVE="${PG_BUILD_DIR}/src/interfaces/libpq/libpq.a"

readonly PG_COMMON_ARCHIVE="${PG_BUILD_DIR}/src/common/libpgcommon_shlib.a"

readonly PG_PORT_ARCHIVE="${PG_BUILD_DIR}/src/port/libpgport_shlib.a"

require_file "${PG_LIBPQ_ARCHIVE}"
require_file "${PG_COMMON_ARCHIVE}"
require_file "${PG_PORT_ARCHIVE}"

# ============================================================
# Install static PostgreSQL libraries
# ============================================================

log "Installing PostgreSQL static archives"

install -d -m 0755 \
  "${PG_STATIC_PREFIX}/lib" \
  "${PG_STATIC_PREFIX}/include" \
  "${PG_STATIC_PREFIX}/lib/pkgconfig"

# Remove archives left by the previous incorrect configuration.
rm -f \
  "${PG_STATIC_PREFIX}/lib/libpgcommon.a" \
  "${PG_STATIC_PREFIX}/lib/libpgport.a"

install -m 0644 \
  "${PG_LIBPQ_ARCHIVE}" \
  "${PG_STATIC_PREFIX}/lib/libpq.a"

install -m 0644 \
  "${PG_COMMON_ARCHIVE}" \
  "${PG_STATIC_PREFIX}/lib/libpgcommon_shlib.a"

install -m 0644 \
  "${PG_PORT_ARCHIVE}" \
  "${PG_STATIC_PREFIX}/lib/libpgport_shlib.a"

# ============================================================
# Install PostgreSQL public headers
# ============================================================

log "Installing PostgreSQL client headers"

install -m 0644 \
  "${PG_SOURCE_DIR}/src/interfaces/libpq/libpq-fe.h" \
  "${PG_STATIC_PREFIX}/include/libpq-fe.h"

install -m 0644 \
  "${PG_SOURCE_DIR}/src/interfaces/libpq/libpq-events.h" \
  "${PG_STATIC_PREFIX}/include/libpq-events.h"

install -m 0644 \
  "${PG_SOURCE_DIR}/src/include/postgres_ext.h" \
  "${PG_STATIC_PREFIX}/include/postgres_ext.h"

# ============================================================
# Verify PostgreSQL public encoding symbols
# ============================================================

log "Verifying PostgreSQL frontend encoding symbols"

require_symbol \
  "${PG_STATIC_PREFIX}/lib/libpgcommon_shlib.a" \
  pg_encoding_to_char

require_symbol \
  "${PG_STATIC_PREFIX}/lib/libpgcommon_shlib.a" \
  pg_char_to_encoding

# ============================================================
# Generate controlled static pkg-config metadata
# ============================================================

log "Generating PostgreSQL static pkg-config metadata"

cat >"${PG_STATIC_PREFIX}/lib/pkgconfig/libpq.pc" <<EOF
prefix=${PG_STATIC_PREFIX}
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libpq
Description: PostgreSQL ${PG_VERSION} static client library
Version: ${PG_VERSION}

Libs: -L\${libdir} -lpq
Libs.private: -lpgcommon_shlib -lpgport_shlib -lssl -lcrypto -lz -lm -ldl -pthread
Cflags: -I\${includedir}
EOF

# The custom PostgreSQL pkg-config file must take priority.
export PKG_CONFIG_PATH="${PG_STATIC_PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

# ============================================================
# Verify PostgreSQL static client
# ============================================================

log "Verifying PostgreSQL static client"

LIBPQ_VERSION="$(pkg-config --modversion libpq)"
LIBPQ_PREFIX="$(pkg-config --variable=prefix libpq)"
LIBPQ_FLAGS="$(pkg-config --static --libs libpq)"

echo "PostgreSQL version: ${LIBPQ_VERSION}"
echo "PostgreSQL prefix:  ${LIBPQ_PREFIX}"
echo "Static flags:       ${LIBPQ_FLAGS}"

[[ "${LIBPQ_VERSION}" == "${PG_VERSION}" ]] ||
  fail "Expected custom libpq ${PG_VERSION}, found ${LIBPQ_VERSION}."

[[ "${LIBPQ_PREFIX}" == "${PG_STATIC_PREFIX}" ]] ||
  fail "pkg-config selected the wrong libpq: ${LIBPQ_PREFIX}"

# All three archives must use the correct names.
for archive in \
  libpq.a \
  libpgcommon_shlib.a \
  libpgport_shlib.a; do

  require_archive "${archive}" "${PG_STATIC_PREFIX}/lib"
done

# Verify that pkg-config exports the correct variants.
grep -Eq '(^|[[:space:]])-lpgcommon_shlib([[:space:]]|$)' \
  <<<"${LIBPQ_FLAGS}" ||
  fail "libpq metadata is missing -lpgcommon_shlib."

grep -Eq '(^|[[:space:]])-lpgport_shlib([[:space:]]|$)' \
  <<<"${LIBPQ_FLAGS}" ||
  fail "libpq metadata is missing -lpgport_shlib."

# Reject the incorrect regular variants.
if grep -Eq \
  '(^|[[:space:]])-lpgcommon([[:space:]]|$)|(^|[[:space:]])-lpgport([[:space:]]|$)' \
  <<<"${LIBPQ_FLAGS}"; then

  fail "Incorrect PostgreSQL internal archive variants detected: ${LIBPQ_FLAGS}"
fi

# Reject optional dependencies disabled at PostgreSQL configure time.
if grep -Eiq 'gssapi|krb5|ldap|curl|pq-oauth|icu|lz4|zstd' \
  <<<"${LIBPQ_FLAGS}"; then

  fail "Unexpected optional dependency in libpq flags: ${LIBPQ_FLAGS}"
fi

# Verify the public pipeline API.
grep -q 'PQpipelineSync' \
  "${PG_STATIC_PREFIX}/include/libpq-fe.h" ||
  fail "PQpipelineSync is missing from custom libpq headers."

echo "Verified PQpipelineSync."
echo "Verified libpgcommon_shlib."
echo "Verified libpgport_shlib."
echo "Verified optional authentication dependencies are disabled."

# ============================================================
# Verify remaining pkg-config modules
# ============================================================

log "Checking pkg-config modules"

for module in libpq liburing libdeflate libssl libcrypto; do
  pkg-config --exists "${module}" ||
    fail "Missing pkg-config module: ${module}"

  printf '%-12s %s\n' \
    "${module}:" \
    "$(pkg-config --modversion "${module}")"
done

# ============================================================
# Configure CMake
# ============================================================

log "Configuring CMake: FULL STATIC + LTO"

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc-14 \
  -DCMAKE_CXX_COMPILER=g++-14 \
  -DHTTPSERVER_NATIVE_ARCH=ON \
  -DHTTPSERVER_FULL_STATIC=ON \
  -DHTTPSERVER_STATIC_PG_PREFIX="${PG_STATIC_PREFIX}"

# ============================================================
# Completion
# ============================================================

log "Dependency installation and configuration completed"

echo "PostgreSQL:       ${LIBPQ_VERSION}"
echo "PostgreSQL libs:  ${PG_STATIC_PREFIX}/lib"
echo "PostgreSQL mode:  STATIC"
echo "OpenSSL:          STATIC"
echo "liburing:         STATIC"
echo "libdeflate:       STATIC"
echo "zlib:             STATIC"
echo "C++ runtime:      STATIC"
echo "Build mode:       FULL STATIC + LTO"

echo
echo "Build with:"
echo "  cmake --build \"${BUILD_DIR}\" --parallel \"\$(nproc)\""

echo
echo "Verify with:"
echo "  file \"${BUILD_DIR}/http_server\""
echo "  readelf -l \"${BUILD_DIR}/http_server\" | grep INTERP"
echo "  readelf -d \"${BUILD_DIR}/http_server\" | grep NEEDED"
