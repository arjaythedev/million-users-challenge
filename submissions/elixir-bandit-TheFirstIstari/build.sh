#!/usr/bin/env bash
# Release build into ./_build/prod/rel/feed. Idempotent; runs as normal user (may download pinned deps).
set -euo pipefail
cd "$(dirname "$0")"
export PATH=/opt/elixir/bin:/opt/otp/bin:$PATH
export LANG=C.UTF-8 LC_ALL=C.UTF-8 ELIXIR_ERL_OPTIONS="+fnu"
export MIX_ENV=prod
mix local.hex --force 2>&1 | tail -1 || true
mix local.rebar --force 2>&1 | tail -1 || true
mix deps.get
mix release --overwrite
echo "built release feed ($(elixir --version 2>&1 | tail -1))"
