#!/usr/bin/env bash
# Runs as a normal user. Fetches the pinned AmmarServer commit and builds ./build/feedapi .
set -euo pipefail
cd "$(dirname "$0")"
HERE="$(pwd)"
BUILD="$HERE/build"
mkdir -p "$BUILD"

# AmmarServer, the HTTP server this submission is built on. Pinned by commit.
# This commit has to be one that carries API mode *and* its io_uring transport ( src/AmmServerlib/threads/
# uringAPIServer.c , apiProtocol.c ) - without them the link below fails with an undefined reference to
# AmmServer_StartAPIOnRingsReusingPort, which is what feedapi.c starts each worker with. It also has to carry
# AmmServer_APIResponse.extraHeaders ( for Content-Encoding: gzip ) and AmmServer_SetAPIBatchHandler ( for
# answering a pass's requests with one pipelined round trip to Postgres ) and the deferred-response API
# ( AmmServer_SetAPINotifyFD / AmmServer_APISendDeferred ) that lets a handler answer after it returns.
AMMARSERVER_REPO="https://github.com/AmmarkoV/AmmarServer.git"
AMMARSERVER_COMMIT="5e5ad865e374bff7f73c37d964e91799790c70a3"

# ---------------------------------------------------------------- AmmarServer
AMMARSERVER="$BUILD/AmmarServer"

if [ -n "${AMMARSERVER_SRC:-}" ]; then
  # Build against a checkout that is already on this machine, for developing the two together. The
  # benchmark box and CI never set this and always build the pinned commit above.
  echo "== AmmarServer from \$AMMARSERVER_SRC=$AMMARSERVER_SRC"
  rm -rf "$AMMARSERVER"
  mkdir -p "$AMMARSERVER"
  cp -a "$AMMARSERVER_SRC/CMakeLists.txt" "$AMMARSERVER_SRC/src" "$AMMARSERVER/"
  # A development checkout usually has object files and libraries lying around in its source tree from
  # previous builds ; none of them are going anywhere near this binary.
  find "$AMMARSERVER/src" -type f \( -name '*.a' -o -name '*.so' -o -name '*.o' \) -delete
else
  echo "== AmmarServer @ $AMMARSERVER_COMMIT"
  if [ ! -d "$AMMARSERVER/.git" ]; then
    rm -rf "$AMMARSERVER"
    git init -q "$AMMARSERVER"
    git -C "$AMMARSERVER" remote add origin "$AMMARSERVER_REPO"
  fi
  git -C "$AMMARSERVER" fetch -q --depth 1 origin "$AMMARSERVER_COMMIT"
  git -C "$AMMARSERVER" -c advice.detachedHead=false checkout -q -f FETCH_HEAD
fi

# Said plainly here rather than left to surface as an undefined reference at the link step below.
for needed in epollAPIServer.c apiProtocol.c uringAPIServer.c; do
  if [ ! -f "$AMMARSERVER/src/AmmServerlib/threads/$needed" ]; then
    echo "build.sh: this AmmarServer checkout has no src/AmmServerlib/threads/$needed , which this" >&2
    echo "          submission is built on ( API mode and its io_uring transport )." >&2
    echo "          AMMARSERVER_COMMIT=$AMMARSERVER_COMMIT needs to point at a commit that carries it." >&2
    exit 1
  fi
done

# The transport batching below is a compile-time option of that commit. Without this check a checkout that
# predates it builds without complaining and the defines silently do nothing, which is a slower server that
# looks like a fast one. URING_BATCH_PACED is the one that changes behaviour rather than a limit.
if ! grep -q "URING_BATCH_PACED" "$AMMARSERVER/src/AmmServerlib/threads/uringAPIServer.c" 2>/dev/null; then
  echo "build.sh: this AmmarServer checkout has no URING_BATCH_PACED in its io_uring transport, so the" >&2
  echo "          batching in TRANSPORT_FLAGS would be silently ignored. AMMARSERVER_COMMIT=$AMMARSERVER_COMMIT" >&2
  echo "          needs to point at a commit that carries the paced batch wait and the send-ticket pool." >&2
  exit 1
fi

# Built with the same optimisation flags as the API itself ( see OPT_FLAGS below ), so that link time
# optimisation can inline AmmarServer's small per-field response writers into the handlers that call them.
# AmmarServer's CMakeLists sets CMAKE_C_FLAGS itself, so they go in through CMAKE_C_FLAGS_RELEASE, which CMake
# appends after it ; and gcc-ar/gcc-ranlib, because an archive of LTO objects needs the GCC plugin to index it.
OPT_FLAGS="-O3 -DNDEBUG -march=native -mtune=native -flto=auto"
# The ring transport's batching, set for this server : while it is busy a pass waits for 256 completions or
# 2 ms ( AmmarServer's default is 32 or 1 ms ), so one pass - one wake-up , one write to Postgres , one round
# trip - serves everything that arrived meanwhile ; and 256 responses may be in flight at once ( default 32 ),
# because a pass now answers that many together.
TRANSPORT_FLAGS="${TRANSPORT_FLAGS:--DURING_BATCH_PACED=1 -DURING_BATCH_WAIT_US=2000 -DURING_BATCH_COMPLETIONS=256 -DURING_SEND_TICKETS=256}"

# Only the AmmarServer library target, not the Services/ demo applications that ship alongside it.
cmake -S "$AMMARSERVER" -B "$AMMARSERVER/build" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS_RELEASE="$OPT_FLAGS $TRANSPORT_FLAGS" \
      -DCMAKE_AR="$(command -v gcc-ar)" -DCMAKE_RANLIB="$(command -v gcc-ranlib)" > "$BUILD/cmake.log"
make -C "$AMMARSERVER/build" -j"$(nproc)" AmmarServer > "$BUILD/make.log"

# The three static archives that build just produced. Named by full path rather than with -L/-l so that
# there is no question of which file ends up in the binary : -lAmmarServer would rather have a
# libAmmarServer.so sitting in the same directory, and nothing prebuilt gets linked here ( rule 12 ).
AMMARSERVER_LIBS=(
  "$AMMARSERVER/src/AmmServerlib/libAmmarServer.a"
  "$AMMARSERVER/src/Hashmap/libHashmap.a"
  "$AMMARSERVER/src/InputParser/libInputParser_C.a"
)
for lib in "${AMMARSERVER_LIBS[@]}"; do
  [ -f "$lib" ] || { echo "build.sh: $lib was not produced by the AmmarServer build" >&2; exit 1; }
done

# ---------------------------------------------------------------- the API itself
# libpq's headers live under their own directory on Debian/Ubuntu ; pg_config is not in libpq-dev, so ask
# pkg-config and fall back to where the package puts them.
PQ_CFLAGS="$(pkg-config --cflags libpq 2>/dev/null || echo -I/usr/include/postgresql)"

# -march=native : everything here is compiled from source on the machine that will run it, so the binary is
# built for that exact CPU. It does mean the binary is only valid on the host that built it - if the
# instruction set under it ever changed, this would be a SIGILL rather than a slow server.
# -flto=auto : AmmarServer's archives above hold LTO objects, so this link optimises across both.
echo "== feedapi"
cc $OPT_FLAGS -Wall \
   $PQ_CFLAGS \
   -I"$AMMARSERVER/src/AmmServerlib" \
   -o "$BUILD/feedapi" \
   feedapi.c jwt.c \
   "${AMMARSERVER_LIBS[@]}" \
   -lpq -lisal -lcrypto -lpthread -lrt -lm

echo "== built $BUILD/feedapi"
