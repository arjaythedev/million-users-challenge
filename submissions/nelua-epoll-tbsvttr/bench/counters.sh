#!/usr/bin/env bash
# In the app container. counters.sh SECONDS: per second over the window, the voluntary context switches of the
# server's threads (its wake-ups), this container's TCP segments in, out and retransmitted (clients and Postgres,
# as /proc/net/snmp counts them), and its CPU in cores (cgroup), user and system. Divide by mload's req/s for the
# hold to get them per request.
sid=$(cat /tmp/server.sid)
snap() {
  local cs
  cs=$(for p in $(ps -eo sid=,pid= | awk -v s="$sid" '$1 == s {print $2}'); do cat /proc/"$p"/task/*/status 2>/dev/null; done |
    awk '/^voluntary_ctxt_switches/ {n += $2} END {print n + 0}')
  echo "$cs $(awk '/^Tcp:/ {if (h) print $11, $12, $13; h = 1}' /proc/net/snmp) $(awk '/^user_usec/ {u = $2} /^system_usec/ {s = $2} END {print u, s}' /sys/fs/cgroup/cpu.stat)"
}
t=${1:-30}
read -r c0 i0 o0 r0 u0 s0 < <(snap); sleep "$t"; read -r c1 i1 o1 r1 u1 s1 < <(snap)
awk -v c=$((c1 - c0)) -v i=$((i1 - i0)) -v o=$((o1 - o0)) -v r=$((r1 - r0)) -v u=$((u1 - u0)) -v s=$((s1 - s0)) -v t="$t" \
  'BEGIN {printf "wakeups/s %d  segs_in/s %d  segs_out/s %d  retrans/s %d  user %.3f  system %.3f  total %.3f cores\n",
          c / t, i / t, o / t, r / t, u / t / 1e6, s / t / 1e6, (u + s) / t / 1e6}'
