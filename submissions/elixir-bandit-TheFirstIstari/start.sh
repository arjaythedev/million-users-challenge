#!/usr/bin/env bash
set -euo pipefail
ulimit -n 1048576 2>/dev/null || true
cd "$(dirname "$0")"
export LANG=C.UTF-8 LC_ALL=C.UTF-8
export MIX_ENV=prod
# BEAM for 1M keep-alive conns + 42k rps (all in-process, no kernel tuning per rules)
# Async I/O threads scale with visible cores (nproc respects taskset affinity).
# +A = core count: validated 42/42 + 200k pass on 16-core testbed (was +A 96,
# changed to +A $CORES in Pass7; 16-core behavior changed 96 -> 16 but re-validated).
CORES=$(nproc 2>/dev/null || echo 16)
case "$CORES" in ''|*[!0-9]*) CORES=16 ;; esac
export ERL_FULLSWEEP_AFTER=0
export ERL_MAX_PORTS=${ERL_MAX_PORTS:-2097152}
export ELIXIR_ERL_OPTIONS="+fnu +P 2097152 +Q 2097152 +A $CORES +K true +sbwt none +sbwtdcpu none +sbwtdio none"
# NOTE: ERL_MEM_TOTAL is not a real OTP var (was a no-op here); memory is bounded
# by the 75%-RAM design intent via pool/connection caps, not an env var. Removed.
: "${DATABASE_URL:?missing DATABASE_URL}"
: "${JWT_SECRET:?missing JWT_SECRET}"
: "${HOST:=0.0.0.0}"
: "${PORT:?missing PORT}"
: "${EXTRA_PORT:?missing EXTRA_PORT}"
: "${DB_POOL:=$(( CORES * 14 ))}"
export DB_POOL
exec ./_build/prod/rel/feed/bin/feed start
