#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p .deps/yyjson build
compiler="${CC:-gcc-13}"
jobs="${BUILD_JOBS:-2}"
fetch() {
    local path="$1" hash="$2" url="$3" temporary
    if ! test -f "$path" || ! printf '%s  %s\n' "$hash" "$path" | sha256sum --status -c -; then
        temporary="$(mktemp "$path.tmp.XXXXXX")"
        if ! curl --fail --location --retry 3 "$url" -o "$temporary"; then
            rm -f "$temporary"
            return 1
        fi
        if ! printf '%s  %s\n' "$hash" "$temporary" | sha256sum -c -; then
            rm -f "$temporary"
            return 1
        fi
        mv -f "$temporary" "$path"
    fi
    printf '%s  %s\n' "$hash" "$path" | sha256sum -c -
}
unpack() {
    local name="$1" archive="$2" temporary
    if ! test -f ".deps/$name/.compact-source"; then
        temporary="$(mktemp -d .deps/unpack.XXXXXX)"
        if ! tar -xf "$archive" -C "$temporary"; then
            rm -rf "$temporary"
            return 1
        fi
        rm -rf ".deps/$name"
        mv "$temporary/$name" ".deps/$name"
        rmdir "$temporary"
        touch ".deps/$name/.compact-source"
    fi
}
fetch .deps/yyjson/yyjson.c 98489c068ab65298320710e5be4c1090af1b5676e41301a3525b2e732641aace https://raw.githubusercontent.com/ibireme/yyjson/0.10.0/src/yyjson.c
fetch .deps/yyjson/yyjson.h adce02aef5dfd868f7c0b632bd23bc0e79fa710b70d93bfe334c2bd15b21a41d https://raw.githubusercontent.com/ibireme/yyjson/0.10.0/src/yyjson.h
fetch .deps/yyjson/LICENSE 45e384d3d52c73cba3a64d6e6c25d47cd738cd8a55c30629e3201046eda62947 https://raw.githubusercontent.com/ibireme/yyjson/0.10.0/LICENSE
fetch .deps/libdeflate-1.23.tar.gz 1ab18349b9fb0ce8a0ca4116bded725be7dcbfa709e19f6f983d99df1fb8b25f https://github.com/ebiggers/libdeflate/archive/refs/tags/v1.23.tar.gz
fetch .deps/postgresql-18.6.tar.bz2 555610c24d53e4316da5b7d3fc25c279d96856d5e0e23ee308c328c5fa881d9f https://ftp.postgresql.org/pub/source/v18.6/postgresql-18.6.tar.bz2
unpack libdeflate-1.23 .deps/libdeflate-1.23.tar.gz
unpack postgresql-18.6 .deps/postgresql-18.6.tar.bz2
CFLAGS='' CPPFLAGS='' LDFLAGS='' cmake -S .deps/libdeflate-1.23 -B .deps/libdeflate-build \
    -DCMAKE_C_COMPILER="$compiler" -DCMAKE_C_FLAGS='' -DCMAKE_C_FLAGS_RELEASE='-O3 -DNDEBUG -march=native' \
    -DCMAKE_BUILD_TYPE=Release -DLIBDEFLATE_BUILD_SHARED_LIB=OFF -DLIBDEFLATE_BUILD_GZIP=OFF
cmake --build .deps/libdeflate-build --parallel "$jobs"
postgres="$PWD/.deps/postgresql-18.6"
if ! test -f "$postgres/src/interfaces/libpq/libpq.a" || \
   ! test -f "$postgres/src/common/libpgcommon_shlib.a" || \
   ! test -f "$postgres/src/port/libpgport_shlib.a"; then
    (
        cd "$postgres"
        CC="$compiler" AR=ar RANLIB=ranlib CFLAGS='-O3 -march=native' CPPFLAGS='' LDFLAGS='' LIBS='' \
            ./configure --prefix="$postgres/install" --without-readline --without-icu --without-gssapi \
                --without-ldap --with-ssl=openssl --without-zlib --without-libcurl
        make -C src/interfaces/libpq -j "$jobs"
    )
fi
read -r -a packages <<< "$(pkg-config --cflags --libs openssl)"
read -r -a extra_c <<< "${CFLAGS:-}"
read -r -a extra_ld <<< "${LDFLAGS:-}"
"$compiler" -std=c17 -O3 -march=native -flto -fno-plt -Wall -Wextra -Wpedantic -Werror -DNDEBUG -DCOMPACT_DB_BATCH \
    -I.deps/yyjson -I.deps/libdeflate-1.23 -isystem "$postgres/src/interfaces/libpq" -isystem "$postgres/src/include" \
    "${extra_c[@]}" server.c db.c auth.c codec.c .deps/yyjson/yyjson.c .deps/libdeflate-build/libdeflate.a \
    -Wl,--start-group "$postgres/src/interfaces/libpq/libpq.a" "$postgres/src/common/libpgcommon_shlib.a" \
        "$postgres/src/port/libpgport_shlib.a" -Wl,--end-group \
    "${packages[@]}" -lm -pthread "${extra_ld[@]}" -o build/server
