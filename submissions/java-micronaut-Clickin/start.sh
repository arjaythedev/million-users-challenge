#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
cpus=$(nproc)
event_loops=${MU_EVENT_LOOPS:-$(( cpus / 2 > 2 ? cpus / 2 : 2 ))}
gzip_level=${MU_GZIP_LEVEL:-$(( cpus >= 8 ? 1 : 0 ))}
gzip_mode=${MU_GZIP_MODE:-loop}
db_loops=${MU_DB_LOOPS:-$(( cpus / 4 > 1 ? cpus / 4 : 1 ))}
db_pool=${MU_DB_POOL:-64}
db_pipeline=${MU_DB_PIPELINE:-256}
printf 'event_loops=%s gzip_level=%s gzip_mode=%s db_loops=%s db_pool_requested=%s db_pipeline=%s\n' "$event_loops" "$gzip_level" "$gzip_mode" "$db_loops" "$db_pool" "$db_pipeline" >&2
exec java --enable-native-access=ALL-UNNAMED "-Dmu.event-loops=$event_loops" "-Dmu.gzip-level=$gzip_level" "-Dmu.gzip-mode=$gzip_mode" "-Dmu.db-loops=$db_loops" "-Dmu.db-pool=$db_pool" "-Dmu.db-pipeline=$db_pipeline" -XX:InitialRAMPercentage=62.5 -XX:MaxRAMPercentage=62.5 -XX:MaxHeapFreeRatio=100 -XX:+UseG1GC -XX:+UseCompactObjectHeaders -jar target/feed.jar
