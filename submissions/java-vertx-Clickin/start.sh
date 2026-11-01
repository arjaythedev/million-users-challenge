#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
cpus=$(nproc)
event_loops=${MU_EVENT_LOOPS:-$cpus}
gzip_level=${MU_GZIP_LEVEL:-$(( cpus >= 8 ? 1 : 0 ))}
db_pool=${MU_DB_POOL:-64}
db_pipeline=${MU_DB_PIPELINE:-256}
heap=${MU_HEAP:-}
printf 'event_loops=%s gzip_level=%s db_pool_requested=%s db_pipeline=%s heap=%s\n' \
  "$event_loops" "$gzip_level" "$db_pool" "$db_pipeline" "${heap:-75% of RAM}" >&2
exec java --enable-native-access=ALL-UNNAMED --sun-misc-unsafe-memory-access=allow \
  -XX:+UseZGC ${heap:+"-Xmx$heap"} -XX:MaxRAMPercentage=75 -XX:-ZUncommit -XX:+UseCompactObjectHeaders \
  -Dvertx.disableHttpHeadersValidation=true -Dio.netty.buffer.checkBounds=false -Dio.netty.buffer.checkAccessible=false \
  "-Dmu.event-loops=$event_loops" "-Dmu.gzip-level=$gzip_level" "-Dmu.db-pool=$db_pool" "-Dmu.db-pipeline=$db_pipeline" \
  -jar target/feed.jar
