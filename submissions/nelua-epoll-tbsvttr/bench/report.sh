#!/usr/bin/env bash
# In the app container, after a run: the sampler's peaks, the mean CPU and RSS while every connection was up (the
# hold), the Postgres backends on the database and the end of the server's log.
awk '{for (i = 2; i <= NF; i++) {split($i, kv, "="); v[NR, kv[1]] = kv[2] + 0; if (kv[2] + 0 > max[kv[1]]) max[kv[1]] = kv[2] + 0}}
     END {
       for (r = 1; r <= NR; r++) if (v[r, "inuse"] >= 0.99 * max["inuse"]) {n++; s += v[r, "cpu_cores"]; m += v[r, "rss_mb"]}
       printf "server peak: cpu_cores=%.2f rss_mb=%d tcp_inuse=%d\n", max["cpu_cores"], max["rss_mb"], max["inuse"]
       if (n) printf "hold (%d samples, all connections up): mean cpu_cores=%.3f mean rss_mb=%d\n", n, s / n, m / n
     }' /tmp/samples.log
echo "pg backends on feed: $(psql -qXAt -d postgres -c "SELECT count(*) FROM pg_stat_activity WHERE datname = 'feed'")"
echo "server log (last 3):"; tail -3 /tmp/server.log
