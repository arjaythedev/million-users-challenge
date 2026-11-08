#!/usr/bin/env bash
# build.sh: runs as a normal user. gforth is interpreted, so there is no binary
# to produce -- but gforth's `c-library` compiles its FFI glue (libpq/crypto/socket
# wrappers) to a shared object on first load. We force that compilation now so the
# server starts instantly and we fail the build early if the FFI link is broken.
set -euo pipefail
cd "$(dirname "$0")"

# ffi.fs includes the libpq header by its Ubuntu absolute path (gforth 0.7.3's
# c-library has no portable -I). Assert it's present so a missing libpq-dev
# fails the build with a clear message instead of a cryptic libtool error.
HDR=/usr/include/postgresql/libpq-fe.h
[ -f "$HDR" ] || { echo "build.sh: $HDR not found -- is libpq-dev installed? (install.sh)"; exit 1; }

# Clear gforth's c-library FFI cache so the wrapper .so is regenerated from the
# CURRENT ffi.fs. gforth caches the compiled wrapper under ~/.gforth/libcc-named
# keyed by the C source; clearing it guarantees build.sh and start.sh link the
# same freshly built wrapper (a stale cache can otherwise miss a newly added
# c-function symbol at server startup). GFORTHHIST/HOME both anchor the cache.
rm -rf "${GFORTHHIST:-${XDG_STATE_HOME:-$HOME/.gforth}}/libcc-named" 2>/dev/null || true
rm -rf "$HOME/.gforth/libcc-named" 2>/dev/null || true

# Build the FFI wrapper + load the whole program (without starting the server).
# `compile-check.fs` requires every layer and prints COMPILED OK.
gforth compile-check.fs

echo "build.sh complete"
