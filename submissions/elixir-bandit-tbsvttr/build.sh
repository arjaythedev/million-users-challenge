#!/usr/bin/env bash
# Build the release: Hex and rebar3 through Mix, the dependencies exactly as pinned in mix.lock, then `mix release`.
set -euo pipefail
cd "$(dirname "$0")"
export PATH=/opt/elixir/bin:/opt/otp/bin:$PATH MIX_ENV=prod
export MIX_HOME="$PWD/.mix" HEX_HOME="$PWD/.hex"
export ELIXIR_ERL_OPTIONS=+fnu   # UTF-8 file names even without a UTF-8 locale (as under systemd)
mix local.hex 2.6.0 --force --if-missing
mix local.rebar --force --if-missing
mix deps.get --only prod --check-locked
mix release --overwrite --quiet
