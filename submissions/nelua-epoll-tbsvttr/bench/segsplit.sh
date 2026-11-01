#!/usr/bin/env bash
# In the app container. segsplit.sh SECONDS: TCP segments per second over the window, split into Postgres sockets
# (peer port 5432) and client sockets, from each established socket's own counters (tcp_info, so incoming segments
# are counted before the kernel merges them). Only sockets open for the whole window count.
snap() {
  ss -tinH state established | awk '
    /^[0-9]/ {peer = $4; key = $3 " " $4; next}
    { so = 0; si = 0
      for (i = 1; i <= NF; i++) {
        if ($i ~ /^segs_out:/) {split($i, a, ":"); so = a[2]}
        if ($i ~ /^segs_in:/) {split($i, a, ":"); si = a[2]}
      }
      print key, (peer ~ /:5432$/ ? "pg" : "client"), so, si }'
}
snap > /tmp/seg0; sleep "${1:-30}"; snap > /tmp/seg1
awk -v t="${1:-30}" 'NR == FNR {o[$1 " " $2] = $4; i[$1 " " $2] = $5; next}
  ($1 " " $2) in o {k = $3; out[k] += $4 - o[$1 " " $2]; in_[k] += $5 - i[$1 " " $2]}
  END {for (k in out) printf "%-6s segs_out/s %d  segs_in/s %d\n", k, out[k] / t, in_[k] / t}' /tmp/seg0 /tmp/seg1
