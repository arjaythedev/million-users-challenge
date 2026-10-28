#!/usr/bin/env bash
# In the load container. wait.sh [HOLDERS]: wait (up to 10 min) until that many idle holders (default 12: six IPs
# times two ports) say "holding", then print their totals.
want=${1:-12}
for _ in $(seq 120); do
  n=$(grep -h holding /tmp/idle-*.log 2>/dev/null | wc -l)
  [ "$n" -ge "$want" ] && break
  sleep 5
done
grep -h holding /tmp/idle-*.log | awk '{held += $4; failed += $6} END {print "holding", held, "failed", failed}'
