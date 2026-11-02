#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export NODE_ENV=production
exec bun dist/main.js
