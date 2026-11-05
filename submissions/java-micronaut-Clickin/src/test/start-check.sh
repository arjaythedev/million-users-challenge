#!/usr/bin/env bash
set -euo pipefail

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
cat > "$dir/nproc" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$TEST_CPUS"
EOF
cat > "$dir/java" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$@"
EOF
chmod +x "$dir/nproc" "$dir/java"

check() {
  local cpus=$1 loops=$2 gzip=$3 mode=$4 db_loops=$5 db_pool=$6 pipeline=$7 output line
  shift 7
  output=$(env -u MU_CARRIERS -u MU_EVENT_LOOPS -u MU_GZIP_LEVEL -u MU_GZIP_MODE -u MU_DB_PERMITS \
    -u MU_DB_LOOPS -u MU_DB_POOL -u MU_DB_PIPELINE PATH="$dir:$PATH" TEST_CPUS="$cpus" "$@" \
    bash "$(dirname "$0")/../../start.sh" 2>&1)
  for line in "event_loops=$loops gzip_level=$gzip gzip_mode=$mode db_loops=$db_loops db_pool_requested=$db_pool db_pipeline=$pipeline" \
    "-Dmu.event-loops=$loops" "-Dmu.gzip-level=$gzip" "-Dmu.gzip-mode=$mode" \
    "-Dmu.db-loops=$db_loops" "-Dmu.db-pool=$db_pool" "-Dmu.db-pipeline=$pipeline"; do
    if ! grep -Fxq -- "$line" <<< "$output"; then
      printf 'Expected %s for %s CPUs; got:\n%s\n' "$line" "$cpus" "$output" >&2
      exit 1
    fi
  done
  if grep -Eq -- '^-D(mu\.db-permits|jdk\.virtualThreadScheduler\.parallelism|mu\.compression-threshold)=' <<< "$output"; then
    printf 'Unexpected obsolete property; got:\n%s\n' "$output" >&2
    exit 1
  fi
}

check 1 2 0 loop 1 64 256
check 4 2 0 loop 1 64 256
check 7 3 0 loop 1 64 256
check 8 4 1 loop 2 64 256
check 9 4 1 loop 2 64 256
check 16 8 1 loop 4 64 256
check 4 3 9 pool 5 128 32 MU_EVENT_LOOPS=3 MU_GZIP_LEVEL=9 MU_GZIP_MODE=pool MU_DB_LOOPS=5 MU_DB_POOL=128 MU_DB_PIPELINE=32
check 16 8 0 loop 4 64 256 MU_GZIP_LEVEL=0 MU_GZIP_MODE=loop
check 8 4 1 loop 2 64 256 MU_GZIP_MODE= MU_DB_LOOPS= MU_DB_POOL= MU_DB_PIPELINE=
check 4 2 0 loop 1 64 256 MU_CARRIERS=8 MU_DB_PERMITS=2
printf 'Startup configuration checks passed\n'
