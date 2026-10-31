#!/bin/sh
# In the load container. holdrps.sh [LOG]: from mload's output, the mean req/s and the worst 5-second p95/p99 over
# the hold (the windows where every user is connected), and the RESULT line.
awk '/^t=/ {for (i = 1; i <= NF; i++) {split($i, kv, "="); f[kv[1]] = kv[2]}
            gsub("ms", "", f["p95"]); gsub("ms", "", f["p99"])
            if (f["conns"] == f["users"] && f["users"] > 0) {n++; s += f["req/s"]; if (f["p95"] + 0 > p95) p95 = f["p95"] + 0; if (f["p99"] + 0 > p99) p99 = f["p99"] + 0}}
     /^RESULT/ {print}
     END {if (n) printf "hold: %d windows, mean req/s=%.0f, worst 5 s window p95=%.1fms p99=%.1fms\n", n, s / n, p95, p99}' "${1:-/tmp/run.log}"
