#!/usr/bin/env bash
# Builds bin/server from authentic Plankalkul source (.pk2d 1942 2D notation + .pk linearised notation):
#   1. Builds Zaneham/plankalkul-compiler (commit a3bc749a, SHA-256 pinned) from OCaml source via Dune.
#   2. Compiles src/zuse_1942.pk2d and src/feed_plans.pk to C via `plankalkul --emit-c`.
#   3. Builds static libpq from PostgreSQL 18.6 and static igzip from Intel ISA-L 2.31.1 (both SHA-256 pinned).
#   4. Links the Plankalkul-generated C plans with src/runtime.c into bin/server.
set -euo pipefail
cd "$(dirname "$0")"

PK_COMMIT=a3bc749a948d99c544b7e2fdcf5137e96465cc6e
PK=plankalkul-compiler-$PK_COMMIT
PKSHA=01979638294cc6d7474e534a2c5b55eaad91b62ebcd77059cd447931579021fb

PG=postgresql-18.6
PGSHA=983ee554ec53dbeb9b70797bef9fcf4e67e117e7e48ca1463cc80b3ff8e8ff3f

IS=isa-l-2.31.1
ISSHA=e1d5573a4019738243b568ab1e1422e6ab7557c5cae33cc8686944d327ad6bb4

if [ "$(uname -m)" = x86_64 ]; then MARCH=-march=native; else MARCH=-mcpu=native; fi
mkdir -p bin build deps

# 1. Build the Plankalkul compiler (Zaneham/plankalkul-compiler) from source
PKC=deps/$PK/_build/default/src/bin/plankalkul.exe
if [ ! -x "$PKC" ]; then
  curl -fsSL -o deps/$PK.tar.gz https://github.com/Zaneham/plankalkul-compiler/archive/$PK_COMMIT.tar.gz
  echo "$PKSHA  deps/$PK.tar.gz" | sha256sum -c -
  rm -rf deps/$PK && tar -xzf deps/$PK.tar.gz -C deps
  # Strip test suite & opam generation so Dune only requires ocaml + ppx_deriving
  rm -rf deps/$PK/test
  printf '(lang dune 3.0)\n(name plankalkul)\n' > deps/$PK/dune-project
  (cd deps/$PK && dune build src/bin/plankalkul.exe)
fi

# 2. Compile Plankalkul 1942 2D notation (.pk2d) and linearised Plans (.pk) to C
echo "Compiling Plankalkul 2D Plans (src/zuse_1942.pk2d) -> build/zuse_1942.c"
"$PKC" --emit-c src/zuse_1942.pk2d > build/zuse_1942.c

echo "Compiling Plankalkul Linear Plans & Wahrheitstafel (src/feed_plans.pk) -> build/feed_plans.c"
"$PKC" --emit-c src/feed_plans.pk > build/feed_plans.c

# 3. Build static libpq from PostgreSQL 18.6 source
if [ ! -f deps/$PG/src/interfaces/libpq/libpq.a ]; then
  curl -fsSL -o deps/$PG.tar.gz https://ftp.postgresql.org/pub/source/v18.6/$PG.tar.gz
  echo "$PGSHA  deps/$PG.tar.gz" | sha256sum -c -
  rm -rf deps/$PG && tar -xzf deps/$PG.tar.gz -C deps
  (cd deps/$PG && CFLAGS="-O2 $MARCH" ./configure -q --without-readline --without-zlib --without-icu \
     --without-openssl --without-gssapi --without-ldap >/dev/null \
   && make -s -C src/interfaces/libpq -j"$(nproc)" all-static-lib >/dev/null)
fi

# 4. Build static Intel ISA-L 2.31.1 (igzip)
if [ ! -f deps/$IS/bin/isa-l.a ]; then
  curl -fsSL -o deps/$IS.tar.gz https://github.com/intel/isa-l/archive/refs/tags/v2.31.1.tar.gz
  echo "$ISSHA  deps/$IS.tar.gz" | sha256sum -c -
  rm -rf deps/$IS && tar -xzf deps/$IS.tar.gz -C deps
  make -s -C deps/$IS -f Makefile.unx -j"$(nproc)" lib >/dev/null
fi

# 5. Compile and link Plankalkul-generated C modules + epoll/libpq/igzip runtime bridge
P=deps/$PG/src
gcc -O3 $MARCH -flto=auto -fno-plt -ffunction-sections -fdata-sections -Wl,--gc-sections -pthread -D_GNU_SOURCE \
  -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-function -Wno-unused-result \
  -include src/pk_bridge.h \
  -Isrc -I$P/interfaces/libpq -I$P/include -Ideps/$IS/include \
  build/zuse_1942.c build/feed_plans.c src/runtime.c \
  $P/interfaces/libpq/libpq.a $P/common/libpgcommon_shlib.a $P/port/libpgport_shlib.a \
  deps/$IS/bin/isa-l.a -lm -o bin/server
