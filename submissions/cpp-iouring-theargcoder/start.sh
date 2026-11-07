#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SERVER="${ROOT}/build/http_server"

: "${DATABASE_URL:?DATABASE_URL must be set}"
: "${JWT_SECRET:?JWT_SECRET must be set}"

if [[ ! -x "${SERVER}" ]]; then
  echo "Executable not found: ${SERVER}" >&2
  exit 1
fi

exec "${SERVER}"
