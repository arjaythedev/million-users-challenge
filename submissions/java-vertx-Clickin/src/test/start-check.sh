#!/usr/bin/env bash
set -euo pipefail
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
cat > "$dir/nproc" <<'MOCK'
#!/usr/bin/env bash
printf '%s\n' "$TEST_CPUS"
MOCK
cat > "$dir/java" <<'MOCK'
#!/usr/bin/env bash
printf '%s\n' "$@"
MOCK
chmod +x "$dir/nproc" "$dir/java"

check() {
  local cpus=$1 loops=$2 gzip=$3 pool=$4 pipeline=$5 heap=$6
  shift 6
  env -u MU_EVENT_LOOPS -u MU_GZIP_LEVEL -u MU_DB_POOL -u MU_DB_PIPELINE -u MU_HEAP \
    PATH="$dir:$PATH" TEST_CPUS="$cpus" "$@" \
    bash "$(dirname "$0")/../../start.sh" > "$dir/args" 2> "$dir/config"
  printf 'event_loops=%s gzip_level=%s db_pool_requested=%s db_pipeline=%s heap=%s\n' \
    "$loops" "$gzip" "$pool" "$pipeline" "${heap:-75% of RAM}" > "$dir/expected-config"
  {
    printf '%s\n' --enable-native-access=ALL-UNNAMED --sun-misc-unsafe-memory-access=allow -XX:+UseZGC
    [[ -z $heap ]] || printf '%s\n' "-Xmx$heap"
    printf '%s\n' -XX:MaxRAMPercentage=75 -XX:-ZUncommit -XX:+UseCompactObjectHeaders \
      -Dvertx.disableHttpHeadersValidation=true -Dio.netty.buffer.checkBounds=false -Dio.netty.buffer.checkAccessible=false \
      "-Dmu.event-loops=$loops" "-Dmu.gzip-level=$gzip" "-Dmu.db-pool=$pool" "-Dmu.db-pipeline=$pipeline" \
      -jar target/feed.jar
  } > "$dir/expected-args"
  diff -u "$dir/expected-config" "$dir/config"
  diff -u "$dir/expected-args" "$dir/args"
}
check 1 1 0 64 256 ''
check 4 4 0 64 256 ''
check 8 8 1 64 256 ''
check 16 16 1 64 256 ''
check 4 3 9 128 32 128m MU_EVENT_LOOPS=3 MU_GZIP_LEVEL=9 MU_DB_POOL=128 MU_DB_PIPELINE=32 MU_HEAP=128m
check 8 8 1 64 256 '' MU_EVENT_LOOPS= MU_GZIP_LEVEL= MU_DB_POOL= MU_DB_PIPELINE= MU_HEAP=
printf 'Startup configuration checks passed\n'
