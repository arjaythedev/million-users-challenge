#!/usr/bin/env bash
# Optional comparison dependency. Builds source locally; does not alter the kernel.
set -euo pipefail
cd "$(dirname "$0")"
VERSION=2.5
SHA256=456f5f882165630f0dc7b75e8fd53bd01a955d5d4720729b4323097e6e9f2a98
mkdir -p .native/lib .native/liburing-2.5
ARCHIVE=".native/liburing-$VERSION.tar.gz"
SOURCE=".native/liburing-$VERSION"
if [ ! -f "$ARCHIVE" ]; then
  curl -fL --retry 3 "https://github.com/axboe/liburing/archive/refs/tags/liburing-$VERSION.tar.gz" -o "$ARCHIVE"
fi
printf '%s  %s\n' "$SHA256" "$ARCHIVE" | sha256sum -c -
tar -xzf "$ARCHIVE" --strip-components=1 -C "$SOURCE"
(
  cd "$SOURCE"
  ./configure --prefix="$PWD/local"
  make -C src -j"$(nproc)"
)
# UringPoller owns an opaque ring and reads only the feature bitfield. Verify its
# pinned x86-64 ABI explicitly instead of assuming host compiler packing.
cat > .native/check-liburing-abi.c <<'C'
#include <stddef.h>
#include <liburing.h>
_Static_assert(sizeof(void *) == 8, "Linux x86-64 pointer ABI required");
_Static_assert(sizeof(struct io_uring) == 216, "io_uring ring ABI changed");
_Static_assert(offsetof(struct io_uring, features) == 200, "io_uring features offset changed");
_Static_assert(sizeof(struct io_uring_cqe) == 16, "io_uring CQE ABI changed");
_Static_assert(offsetof(struct io_uring_cqe, res) == 8, "io_uring CQE result offset changed");
int main(void) { return 0; }
C
cc -I"$SOURCE/src/include" .native/check-liburing-abi.c -o .native/check-liburing-abi
.native/check-liburing-abi
install -m 755 "$SOURCE/src/liburing-ffi.so.2.5" .native/lib/liburing-ffi.so.2
echo "Built pinned liburing-ffi 2.5 with matching ABI; copy .native/lib/liburing-ffi.so.2 beside the published app."
