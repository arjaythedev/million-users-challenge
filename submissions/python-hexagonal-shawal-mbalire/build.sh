#!/usr/bin/env bash
# Builds as a normal user: a venv with the pinned wheels, the libpq C extension, then the unit tests.
set -euo pipefail
cd "$(dirname "$0")"
PYTHON="${PYTHON:-python3}"
[ -x .venv/bin/python ] || "$PYTHON" -m venv .venv
PY=.venv/bin/python
"$PY" -m pip install --disable-pip-version-check --no-input --quiet --only-binary=:all: -r requirements.txt

SUFFIX="$("$PY" -c 'import sysconfig; print(sysconfig.get_config_var("EXT_SUFFIX"))')"
PYINC="$("$PY" -c 'import sysconfig; print(sysconfig.get_paths()["include"])')"
if pkg-config --exists libpq 2>/dev/null; then
  PQ_CFLAGS="$(pkg-config --cflags libpq)"; PQ_LIBS="$(pkg-config --libs libpq)"
else  # no libpq.pc (e.g. a non-Debian dev machine): ask pg_config, and remember where libpq lives
  PQ_CFLAGS="-I$(pg_config --includedir)"; PQ_LIBS="-L$(pg_config --libdir) -Wl,-rpath,$(pg_config --libdir) -lpq"
fi
# libdeflate (response gzip) from its source tarball, SHA-256 pinned, compiled into the extension.
LD=libdeflate-1.26 LDSHA=bba03fffc5538576213675ce6968fcff6ce2e67d82e4d5febea2d05f9f13cf85
if [ ! -f deps/$LD/libdeflate.h ]; then
  mkdir -p deps
  "$PY" -c 'import sys, urllib.request; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])' \
    https://github.com/ebiggers/libdeflate/archive/refs/tags/v1.26.tar.gz deps/$LD.tar.gz
  echo "$LDSHA  deps/$LD.tar.gz" | sha256sum -c --quiet -
  rm -rf deps/$LD && tar -xzf deps/$LD.tar.gz -C deps
fi
LD_SRC=(deps/$LD/lib/{deflate_compress,gzip_compress,crc32,utils}.c deps/$LD/lib/x86/cpu_features.c deps/$LD/lib/arm/cpu_features.c)

CFLAGS=(-O2 -fPIC -shared -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I"$PYINC")
# shellcheck disable=SC2086
cc "${CFLAGS[@]}" $PQ_CFLAGS feed/adapters/outbound/postgres/_pq.c -o "feed/adapters/outbound/postgres/_pq$SUFFIX" $PQ_LIBS
cc "${CFLAGS[@]}" feed/adapters/outbound/json/_cjson.c -o "feed/adapters/outbound/json/_cjson$SUFFIX"
cc "${CFLAGS[@]}" -Ideps/$LD feed/adapters/inbound/_deflate.c "${LD_SRC[@]}" -o "feed/adapters/inbound/_deflate$SUFFIX"

"$PY" -m unittest discover -q -s tests -t .
"$PY" -c 'import sys, uvloop, asyncpg, fastapi, granian, orjson, pydantic
from feed.adapters.outbound.postgres import _pq
from feed.adapters.inbound import _deflate
print("python", sys.version.split()[0], "uvloop", uvloop.__version__, "asyncpg", asyncpg.__version__,
      "fastapi", fastapi.__version__, "granian", granian.__version__, "orjson", orjson.__version__,
      "pydantic", pydantic.VERSION, "libpq", _pq.libpq_version())'
