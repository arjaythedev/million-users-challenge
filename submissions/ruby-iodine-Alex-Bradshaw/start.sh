#!/usr/bin/env bash
# Runs the server in the foreground (systemd ExecStart).
# Env: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT. Optional: WORKERS (default: one per core), THREADS (default 6).
set -euo pipefail
cd "$(dirname "$0")"
export PATH="${RUBY_PREFIX:-/opt/ruby-4.0.9}/bin:$PATH"

# glibc malloc: few arenas per process keeps RSS down with a Ruby thread pool in every worker.
export MALLOC_ARENA_MAX="${MALLOC_ARENA_MAX:-2}"
export RUBY_YJIT_ENABLE=1

exec bundle exec ruby server.rb
