#!/usr/bin/env bash
# In the app container. sample.sh SID: every 5 s, the container's CPU in cores (cgroup), the RSS of the server's
# processes (session SID) and the TCP sockets in use.
sid=${1:?usage: sample.sh SID}
usage() { awk '/^usage_usec/ {print $2}' /sys/fs/cgroup/cpu.stat; }
prev=$(usage); pt=$(date +%s%N)
while sleep 5; do
  now=$(usage); t=$(date +%s%N)
  cpu=$(awk -v a="$prev" -v b="$now" -v t0="$pt" -v t1="$t" 'BEGIN {printf "%.2f", (b - a) * 1000 / (t1 - t0)}')
  prev=$now; pt=$t
  rss=$(ps -eo sid=,rss= | awk -v s="$sid" '$1 == s {r += $2} END {printf "%d", r / 1024}')
  tcp=$(awk '/^TCP:/ {print "inuse=" $3 " mem_pages=" $NF}' /proc/net/sockstat)
  echo "$(date +%T) cpu_cores=$cpu rss_mb=$rss $tcp"
done
