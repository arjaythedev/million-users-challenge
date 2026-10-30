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
CFLAGS=(-O2 -fPIC -shared -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I"$PYINC")
# shellcheck disable=SC2086
cc "${CFLAGS[@]}" $PQ_CFLAGS feed/adapters/outbound/postgres/_pq.c -o "feed/adapters/outbound/postgres/_pq$SUFFIX" $PQ_LIBS
cc "${CFLAGS[@]}" feed/adapters/outbound/json/_cjson.c -o "feed/adapters/outbound/json/_cjson$SUFFIX"

"$PY" -m unittest discover -q -s tests -t .
"$PY" -c 'import sys, uvloop, asyncpg, fastapi, granian, orjson, pydantic
from feed.adapters.outbound.postgres import _pq
print("python", sys.version.split()[0], "uvloop", uvloop.__version__, "asyncpg", asyncpg.__version__,
      "fastapi", fastapi.__version__, "granian", granian.__version__, "orjson", orjson.__version__,
      "pydantic", pydantic.VERSION, "libpq", _pq.libpq_version())'
