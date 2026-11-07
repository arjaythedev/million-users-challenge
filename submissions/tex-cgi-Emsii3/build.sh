#!/usr/bin/env bash
# Builds bin/host (C + libpq) and the precompiled pdfTeX format bin/feed.fmt. Idempotent.
set -euo pipefail
cd "$(dirname "$0")"
for d in /opt/homebrew/opt/postgresql@18/bin /usr/local/opt/postgresql@18/bin /usr/lib/postgresql/18/bin; do
  [ -d "$d" ] && PATH="$d:$PATH"
done
export PATH
mkdir -p bin
INC="$(pg_config --includedir)"; LIB="$(pg_config --libdir)"
rm -f bin/pgq
gcc -O2 -Wall -I"$INC" -o bin/host host/host.c -L"$LIB" -Wl,-rpath,"$LIB" -lpq
if [ -f tex/build-fmt.tex ]; then
  pdftex -ini -etex -8bit -jobname=feed -output-directory=bin -interaction=batchmode -halt-on-error tex/build-fmt.tex >/dev/null \
    || { echo "format build failed, see bin/feed.log" >&2; tail -20 bin/feed.log >&2; exit 1; }
else
  echo "warning: tex/build-fmt.tex missing, skipping bin/feed.fmt" >&2
fi
echo "built bin/host$([ -f bin/feed.fmt ] && echo ' bin/feed.fmt')"
