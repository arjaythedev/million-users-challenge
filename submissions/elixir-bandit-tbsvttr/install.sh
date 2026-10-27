#!/usr/bin/env bash
# Install Erlang/OTP into /opt/otp (the Hex team's prebuilt build for Ubuntu 24.04, the one setup-beam, asdf and
# mise install) and Elixir into /opt/elixir (the official precompiled zip), both SHA-256 pinned. Idempotent; run
# as root. x86_64 is the benchmark box; aarch64 is for local runs (Docker on an ARM Mac).
set -euo pipefail
OTP_VERSION=29.1.2
ELIXIR_VERSION=1.20.4
ELIXIR_SHA256=7863c546cda13fecc949e562e326042451dacf8fd8698a36783cb71eeb223b46  # elixir-otp-29.zip
case "$(uname -m)" in   # SHA-256 from builds.hex.pm's builds.txt
  x86_64)  OTP_URL=https://builds.hex.pm/builds/otp/ubuntu-24.04/OTP-$OTP_VERSION.tar.gz
           OTP_SHA256=2cfca446976fc61962585a1c77e5d11ddb6f5fed83e84668a2c4acfaad60d22e ;;
  aarch64) OTP_URL=https://builds.hex.pm/builds/otp/arm64/ubuntu-24.04/OTP-$OTP_VERSION.tar.gz
           OTP_SHA256=5d344f2ef455cfa15ad3112ce3aaecc8fa0d97532b2f106df46bc1701bc02c27 ;;
  *) echo "unsupported architecture $(uname -m)"; exit 1 ;;
esac

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq curl ca-certificates unzip libssl3t64 libncurses6 >/dev/null
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

if [ "$(cat /opt/otp/RELEASE_VERSION 2>/dev/null)" = "$OTP_VERSION" ]; then
  echo "OTP $OTP_VERSION already in /opt/otp"
else
  curl -fsSL -o "$tmp/otp.tar.gz" "$OTP_URL"
  echo "$OTP_SHA256  $tmp/otp.tar.gz" | sha256sum -c -
  rm -rf /opt/otp && mkdir -p /opt/otp
  tar -xzf "$tmp/otp.tar.gz" -C /opt/otp --strip-components=1
  /opt/otp/Install -minimal /opt/otp >/dev/null
  echo "$OTP_VERSION" > /opt/otp/RELEASE_VERSION
fi

if [ "$(cat /opt/elixir/RELEASE_VERSION 2>/dev/null)" = "$ELIXIR_VERSION" ]; then
  echo "Elixir $ELIXIR_VERSION already in /opt/elixir"
else
  curl -fsSL -o "$tmp/elixir.zip" "https://github.com/elixir-lang/elixir/releases/download/v$ELIXIR_VERSION/elixir-otp-29.zip"
  echo "$ELIXIR_SHA256  $tmp/elixir.zip" | sha256sum -c -
  rm -rf /opt/elixir && mkdir -p /opt/elixir
  unzip -q "$tmp/elixir.zip" -d /opt/elixir
  echo "$ELIXIR_VERSION" > /opt/elixir/RELEASE_VERSION
fi

PATH=/opt/elixir/bin:/opt/otp/bin:$PATH elixir --version
