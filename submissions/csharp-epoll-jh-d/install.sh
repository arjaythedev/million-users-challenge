#!/usr/bin/env bash
# Run once as root on Ubuntu 24.04 (safe to repeat): libdeflate for gzip, and .NET SDK 10.0.403 (runtime 10.0.14)
# under /usr/share/dotnet. The SDK version is pinned here and in global.json.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
SDK=10.0.403

packages=()
ldconfig -p | grep -q 'libdeflate\.so\.0' || packages+=(libdeflate0)
sdk_ok() { [ -x /usr/share/dotnet/dotnet ] && /usr/share/dotnet/dotnet --list-sdks | grep -q "^$SDK "; }
sdk_ok || packages+=(curl ca-certificates libicu74)
if (( ${#packages[@]} )); then
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends "${packages[@]}" >/dev/null
fi

if ! sdk_ok; then
  script=$(mktemp)
  curl --fail --silent --show-error --location https://dot.net/v1/dotnet-install.sh --output "$script"
  bash "$script" --version "$SDK" --install-dir /usr/share/dotnet
  rm -f "$script"
  ln -sf /usr/share/dotnet/dotnet /usr/local/bin/dotnet
fi
/usr/share/dotnet/dotnet --list-sdks
