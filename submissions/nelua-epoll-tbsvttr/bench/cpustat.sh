#!/usr/bin/env bash
# cpustat.sh SECONDS FIRST LAST: how busy CPUs FIRST..LAST were over the window, from /proc/stat (the whole VM's
# counters, so the kernel's network work counts wherever it ran, softirqs included).
t=${1:-30}; first=${2:-0}; last=${3:-3}
snap() { awk -v a="$first" -v b="$last" '$1 ~ /^cpu[0-9]+$/ {n = substr($1, 4) + 0; if (n >= a && n <= b) print n, $2 + $3 + $4 + $7 + $8, $5 + $6, $8}' /proc/stat; }
a=$(mktemp); b=$(mktemp)
snap > "$a"; sleep "$t"; snap > "$b"
awk -v t="$t" 'NR == FNR {busy[$1] = $2; idle[$1] = $3; soft[$1] = $4; next}
  {db = $2 - busy[$1]; di = $3 - idle[$1]; ds = $4 - soft[$1]; tb += db; ts += ds
   printf "cpu%d %.0f%% (softirq %.0f%%)  ", $1, 100 * db / (db + di), 100 * ds / (db + di)}
  END {printf "\nbusy %.3f cores, of it softirq %.3f\n", tb / t / 100, ts / t / 100}' "$a" "$b"
rm -f "$a" "$b"
