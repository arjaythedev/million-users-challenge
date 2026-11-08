#!/usr/bin/env bash
# Fetch the dependencies pinned in corral.json/lock.json and compile an optimized binary, build/feed.
set -euo pipefail
cd "$(dirname "$0")"
export PATH=/opt/ponyc/bin:/opt/corral/bin:$PATH
corral fetch
# Ubuntu 24.04 ships OpenSSL 3.0 (the stdlib's crypto and net packages link it)
corral run -- ponyc -Dopenssl_3.0.x --output build feed
