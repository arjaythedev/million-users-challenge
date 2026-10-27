#!/usr/bin/env bash

set -euo pipefail

CMAKE_VERSION=3.31.4
PREFIX=/opt/cmake
SRC=/tmp/cmake-src
PGO=/tmp/pgo

apt-get update
apt-get install -y --no-install-recommends \
    build-essential binutils libssl-dev \
    libevent-dev libpq-dev \
    gzip openssl ca-certificates curl \
    libncurses-dev zlib1g-dev \
    cmake    # just boostrap

# install CMake from source
rm -rf "$SRC" "$PGO"
mkdir -p "$SRC"
curl -fsSL "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}.tar.gz" \
  | tar -xz -C "$SRC" --strip-components=1

cd "$SRC"

FLAGS_C="-O3 -march=native -mtune=native -DNDEBUG -fno-plt -fno-semantic-interposition"
FLAGS_CXX="${FLAGS_C} -flto=auto -fuse-linker-plugin"

./bootstrap --prefix="${PREFIX}" \
    --parallel="$(nproc)" \
    --system-libs \
    --no-system-libarchive \
    --no-system-librhash \
    -- \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="${FLAGS_C} -fprofile-generate=${PGO}" \
    -DCMAKE_CXX_FLAGS_RELEASE="${FLAGS_CXX} -fprofile-generate=${PGO}"

make -j"$(nproc)"
make install


TRAIN=/tmp/pgo-train
mkdir -p "$TRAIN" && cd "$TRAIN"
cat > train.cmake <<'EOF'
string(JSON x GET "{\"a\":1,\"b\":[1,2,3]}" a)
string(REGEX MATCH "^/posts/([0-9]+)$" _ "/posts/123")
string(REPLACE "\r\n" "\n" req "GET /feed HTTP/1.1\r\nHost: x\r\n\r\n")
foreach(i RANGE 1 200)
  string(JSON out SET "{}" "id" 1 "body" "hello" "like_count" 0 "author" "u")
  string(JSON out SET "${out}" "created_at" "2025-12-31T23:59:19.409Z")
endforeach()
EOF
for i in $(seq 1 300); do "${PREFIX}/bin/cmake" -P train.cmake >/dev/null; done

cd "$SRC"
rm -rf CMakeCache.txt CMakeFiles
./bootstrap --prefix="${PREFIX}" \
    --parallel="$(nproc)" \
    --system-libs \
    --no-system-libarchive \
    -- \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="${FLAGS_C} -fprofile-use=${PGO} -fprofile-correction" \
    -DCMAKE_CXX_FLAGS_RELEASE="${FLAGS_CXX} -fprofile-use=${PGO} -fprofile-correction"

make -j"$(nproc)"
make install

"${PREFIX}/bin/cmake" --version



: '

#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

apt-get update
apt-get install -y --no-install-recommends \
    build-essential binutils nasm \
    libevent-dev libpq-dev libssl-dev \
    openssl gzip ca-certificates curl cmake

'