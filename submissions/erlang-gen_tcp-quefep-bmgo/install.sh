#!/usr/bin/env bash
# Installs Erlang/OTP and rebar3 on Ubuntu 24.04 x86_64. Idempotent; run as root.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

if ! command -v erl >/dev/null 2>&1; then
  apt-get update -qq
  apt-get install -y -qq erlang-nox
fi

if ! command -v curl >/dev/null 2>&1 || ! dpkg -s ca-certificates >/dev/null 2>&1; then
  apt-get update -qq
  apt-get install -y -qq ca-certificates curl
fi

# rebar.lock is a 1.2.0 lock file. Keep the builder on the same rebar3 used
# locally instead of whatever the distro happens to ship.
REBAR3_VERSION=3.24.0
if ! command -v rebar3 >/dev/null 2>&1; then
  apt-get update -qq
  apt-get install -y -qq rebar3 || true
fi
if ! rebar3 --version 2>/dev/null | grep -q "$REBAR3_VERSION"; then
  curl -fsSL "https://github.com/erlang/rebar3/releases/download/${REBAR3_VERSION}/rebar3" \
    -o /usr/local/bin/rebar3
  chmod +x /usr/local/bin/rebar3
fi

erl -noshell -eval 'io:format("Erlang/OTP ~s~n", [erlang:system_info(otp_release)]), halt().'
rebar3 --version
