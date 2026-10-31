#!/usr/bin/env bash
# Creates .venv with the pinned wheels (idempotent: re-running re-syncs the pins).
set -euo pipefail
cd "$(dirname "$0")"
[ -x .venv/bin/python ] || python3 -m venv .venv
.venv/bin/pip install --disable-pip-version-check --no-input --quiet --only-binary=:all: -r requirements.txt
.venv/bin/python -c 'import asyncpg, isal, orjson, uvloop, sys; print("python", sys.version.split()[0], "asyncpg", asyncpg.__version__, "uvloop", uvloop.__version__, "orjson", orjson.__version__, "isal", isal.__version__)'
