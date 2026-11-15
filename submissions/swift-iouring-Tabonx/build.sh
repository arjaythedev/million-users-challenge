#!/usr/bin/env bash
# Builds bin/server as a normal user: liburing 2.15 and ISA-L 2.31.1 from source (SHA-256 checked), then the Swift
# sources with swiftc, linked statically against the Swift runtime.
set -euo pipefail
cd "$(dirname "$0")"
BIN=/opt/swift/usr/bin
[ -x "$BIN/swiftc" ] || BIN="$(dirname "$(command -v swiftc)")"
DEPS="$PWD/.deps"; OUT="$PWD/.build"; mkdir -p "$DEPS" "$OUT" bin

fetch() { # fetch <url> <file> <sha256>
    [ -f "$2" ] || curl -fsSL "$1" -o "$2"
    echo "$3  $2" | sha256sum -c - >/dev/null
}
URING_VER=2.15
fetch "https://github.com/axboe/liburing/archive/refs/tags/liburing-$URING_VER.tar.gz" "$DEPS/liburing.tar.gz" \
    8d052f2622dcb3678cbaee5ff582a87572672a6c0a56533cdda5b65cb636120a
URING="$DEPS/uring"
if [ ! -f "$URING/lib/liburing.a" ]; then
    tar xzf "$DEPS/liburing.tar.gz" -C "$DEPS"
    (cd "$DEPS/liburing-liburing-$URING_VER" && CC="$BIN/clang" ./configure --prefix="$URING" >/dev/null &&
     make -j"$(nproc)" -s -C src install >/dev/null)
fi

ISAL_VER=2.31.1
fetch "https://github.com/intel/isa-l/archive/refs/tags/v$ISAL_VER.tar.gz" "$DEPS/isa-l.tar.gz" \
    e1d5573a4019738243b568ab1e1422e6ab7557c5cae33cc8686944d327ad6bb4
ISAL="$DEPS/isa-l-$ISAL_VER"
if [ ! -f "$ISAL/bin/isa-l.a" ]; then
    tar xzf "$DEPS/isa-l.tar.gz" -C "$DEPS"
    make -s -C "$ISAL" -f Makefile.unx -j"$(nproc)" CC="$BIN/clang" lib >/dev/null
fi

"$BIN/clang" -O2 -I"$ISAL/include" -ISources/CShim/include -c Sources/CShim/shim.c -o "$OUT/shim.o"
"$BIN/clang" -O2 -I"$URING/include" -ISources/CUring/include -c Sources/CUring/tu.c -o "$OUT/tu.o"
"$BIN/swiftc" -wmo -Ounchecked -enforce-exclusivity=unchecked -parse-as-library -swift-version 6 -module-name Server \
    -static-stdlib \
    -I Sources/CShim/include -I Sources/CUring/include \
    -Xcc -ISources/CShim/include -Xcc -ISources/CUring/include -Xcc -I"$URING/include" \
    Sources/Server/*.swift "$OUT/shim.o" "$OUT/tu.o" -lpthread "$URING/lib/liburing.a" "$ISAL/bin/isa-l.a" -o bin/server
echo "built bin/server"
