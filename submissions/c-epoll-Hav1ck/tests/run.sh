#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p bin
flags=(-O1 -g -march=x86-64-v2 -pthread -Wall -Wno-unused-result -DNO_HEDGE -DFOUR_CORE_WORKERS=2
       -fsanitize=address,undefined -fno-omit-frame-pointer
       -Ideps/isa-l-2.31.1/include)
for mode in simd scalar; do
  extra=()
  if [ "$mode" = scalar ]; then extra+=(-U__SSE2__); fi
  gcc "${flags[@]}" "${extra[@]}" tests/hotpaths.c deps/isa-l-2.31.1/bin/isa-l.a -o "bin/hotpaths-$mode"
  "bin/hotpaths-$mode"
  gcc "${flags[@]}" "${extra[@]}" tests/http-scan.c deps/isa-l-2.31.1/bin/isa-l.a -o "bin/http-scan-$mode"
  "bin/http-scan-$mode"
done
python3 - <<'PY'
import gzip
from pathlib import Path
assert gzip.decompress(Path('bin/hotpaths.gz').read_bytes()) == Path('bin/hotpaths.raw').read_bytes()
print('ok gzip: independent Python decoder validates the custom Huffman member')
PY
gcc "${flags[@]}" tests/regression.c deps/isa-l-2.31.1/bin/isa-l.a -o bin/regression
./bin/regression
python3 tests/auth-vectors.py > bin/auth-vectors.tsv
for test in auth startup; do
  gcc "${flags[@]}" "tests/$test.c" deps/isa-l-2.31.1/bin/isa-l.a -o "bin/$test"
done
./bin/auth bin/auth-vectors.tsv
./bin/startup

gcc "${flags[@]}" tests/cadence.c deps/isa-l-2.31.1/bin/isa-l.a -o bin/cadence-asan
./bin/cadence-asan
gcc -O2 -g -march=x86-64-v2 -pthread -Wall -Wno-unused-result -DNO_HEDGE -DFOUR_CORE_WORKERS=2 -Ideps/isa-l-2.31.1/include tests/cadence.c deps/isa-l-2.31.1/bin/isa-l.a -o bin/cadence
./bin/cadence

gcc "${flags[@]}" tests/transport.c deps/isa-l-2.31.1/bin/isa-l.a -o bin/transport-asan
./bin/transport-asan
gcc -O2 -g -march=x86-64-v2 -pthread -Wall -Wno-unused-result -DNO_HEDGE -DFOUR_CORE_WORKERS=2 -Ideps/isa-l-2.31.1/include tests/transport.c deps/isa-l-2.31.1/bin/isa-l.a -o bin/transport
./bin/transport
# Exercise the same binary every-split corpus with the scalar HTTP scanner.
gcc "${flags[@]}" -U__SSE2__ tests/transport.c deps/isa-l-2.31.1/bin/isa-l.a -o bin/transport-scalar-asan
./bin/transport-scalar-asan

# Exercise actual startup with both the disabled and normal four-CPU policies.
for workers in 0 2; do
  gcc "${flags[@]}" -UFOUR_CORE_WORKERS -DFOUR_CORE_WORKERS="$workers" tests/workers.c deps/isa-l-2.31.1/bin/isa-l.a -o "bin/workers-$workers-asan"
  "bin/workers-$workers-asan"
  gcc -O2 -g -march=x86-64-v2 -pthread -Wall -Wno-unused-result -DNO_HEDGE -DFOUR_CORE_WORKERS="$workers" -Ideps/isa-l-2.31.1/include tests/workers.c deps/isa-l-2.31.1/bin/isa-l.a -o "bin/workers-$workers"
  "bin/workers-$workers"
done
