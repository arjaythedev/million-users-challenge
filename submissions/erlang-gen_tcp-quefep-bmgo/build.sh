#!/usr/bin/env bash
# Fetches the pinned Hex dependencies and compiles the OTP application. Idempotent.
set -euo pipefail
cd "$(dirname "$0")"
rebar3 compile
