#!/usr/bin/env bash
# Creates .venv with the pinned dependencies (idempotent: re-running re-syncs the pins).
set -euo pipefail
cd "$(dirname "$0")"
[ -x .venv/bin/python ] || python3 -m venv .venv
.venv/bin/pip install --disable-pip-version-check --no-input --quiet --only-binary=:all: -r requirements.txt
.venv/bin/python -c 'import fastapi, uvicorn, gunicorn, orjson, isal, uvloop, httptools, asyncpg; print("fastapi", fastapi.__version__, "uvicorn", uvicorn.__version__, "gunicorn", gunicorn.__version__, "asyncpg", asyncpg.__version__)'
