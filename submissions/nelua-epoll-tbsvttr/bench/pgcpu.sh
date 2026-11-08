#!/bin/sh
# In the Postgres container. pgcpu.sh SECONDS: the container's CPU in cores over the window (cgroup).
usage() { awk '/^usage_usec/ {print $2}' /sys/fs/cgroup/cpu.stat; }
t=${1:-30}
a=$(usage); sleep "$t"; b=$(usage)
awk -v a="$a" -v b="$b" -v t="$t" 'BEGIN {printf "postgres %.3f cores\n", (b - a) / t / 1e6}'
