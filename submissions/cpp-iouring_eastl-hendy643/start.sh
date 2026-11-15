#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
ulimit -n "$(ulimit -Hn)"
exec build/release/server
