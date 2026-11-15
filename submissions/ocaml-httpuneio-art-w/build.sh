#!/usr/bin/env bash
# Release build (idempotent). A local opam switch in _opam/ with every package at the version in
# feed-api.opam.locked, from opam-repository pinned to one commit (first run: ~10 minutes). opam's
# own state (its root) is in _opam-root/: nothing is written outside this folder.
# Output: _build/default/app/main.exe
set -euo pipefail
cd "$(dirname "$0")"
REPO_COMMIT=720105d4adda05127bdf85f10de3e3d2bf2b05b9
REPO=opam-repository-${REPO_COMMIT:0:12}
REPO_URL=git+https://github.com/ocaml/opam-repository#$REPO_COMMIT
export OPAMYES=1 OPAMCONFIRMLEVEL=unsafe-yes OPAMROOT="$PWD/_opam-root"
export DUNE_CACHE=disabled # its default is ~/.cache/dune
[ -d "$OPAMROOT" ] || opam init --bare --disable-sandboxing --no-setup "$REPO" "$REPO_URL"
opam switch list --short | grep -qxF "$PWD" || opam switch create . --empty --repos="$REPO=$REPO_URL"
opam install --switch=. . --deps-only --locked
opam exec --switch=. -- dune build --root . --profile release app/main.exe
