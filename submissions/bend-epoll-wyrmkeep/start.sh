#!/usr/bin/env bash
# Starts route A′'s server: WORKERS processes of .local/build/bend-epoll under the root (SERVER_BIN
# replaces the binary), worker K with --worker K --workers N. They share PORT and EXTRA_PORT through
# SO_REUSEPORT. One file, three callers: the challenge's test/run.sh and the scorer's unit run it as a
# submission's start.sh, and the repository's lab-run.sh and preflight.sh start route A′ through it.
# The root is the nearest directory, from this script's own upward, that holds host/pgwire.c: the
# repository root here, and the folder itself for the copy of this file that assemble.sh puts at
# the top of a submission folder.
#   DATABASE_URL, JWT_SECRET, PORT (3000), EXTRA_PORT (0: none)   as run.sh and the box set them. HOST is
#                                                                  not read: the server listens on every address
# Every other value has its default in the defaults block below, and nowhere else:
#   WORKERS    processes, default nproc: the CPUs this script may run on
#   POOL       database connections per process, default 16, the value the lab measured (at four
#              workers). A process exits 64 when (POOL + 1) x WORKERS is over 512 (one cancel
#              connection per process): with POOL=16, above 30 workers
#   SOCK_OPTS  --sock-opts, default off
#   WINDOW_US  --window-us, default 2000
#   DB         --db, default pg
#   BM_GZIP    --gzip, default isal: ISA-L's level 0 stream (fixed and libdeflate are the others)
#   BM_ROWS    --rows, default block: a feed's rows cross to the Bend code as one cell (each: one
#              per row)
#   BM_RING    --ring, default lifo: under a backlog a worker answers the newest request first
#              (fifo: the oldest first, the binary's own default). These three have a prefix
#              because gzip(1) reads a variable named GZIP
#   BENDRTS    the Bend runtime's flags, default --threads 1 (set, even empty, it stays): the server
#              makes no parallel call, and with one thread the evaluator keeps its continuations on
#              the stack rather than the heap (the repository's bench/servers/bend-epoll/README.md, Workers)
# Arguments go to every worker, after the flags this script passes, so an argument wins over a
# default (lab-run.sh: --batch-wait US --stats FILE; each worker writes FILE.K, and --trace FILE
# likewise). Foreground. SIGTERM or SIGINT goes to every worker. When one worker exits, the others
# get SIGTERM, and the script exits with that worker's status.
# Before it starts any worker it refuses (exit 2) a PORT or EXTRA_PORT that already accepts a
# connection on 127.0.0.1: that listener is not one of its workers, and with SO_REUSEPORT the
# kernel would hand it a share of the connections without an error.
set -euo pipefail
root="$(cd "$(dirname "$0")" && pwd)"
while [[ ! -f $root/host/pgwire.c ]]; do
	[[ $root != / ]] || { echo "${0##*/}: no host/pgwire.c above $0" >&2; exit 2; }
	root="$(dirname "$root")"
done

# Defaults. Each knob is set here. A flag given after this block (from "$@") wins, because
# the binary keeps the last value of a repeated flag (ep_flags in host/epoll.c).
WORKERS=${WORKERS:-$(nproc)}   # processes; nproc: the number of processing units available to this script
POOL=${POOL:-16}               # database connections per process
SOCK_OPTS=${SOCK_OPTS:-off}    # --sock-opts on|off
WINDOW_US=${WINDOW_US:-2000}   # --window-us
DB=${DB:-pg}                   # --db
BM_GZIP=${BM_GZIP:-isal}       # --gzip isal|fixed|libdeflate
BM_ROWS=${BM_ROWS:-block}      # --rows block|each
BM_RING=${BM_RING:-lifo}       # --ring lifo|fifo
export BENDRTS=${BENDRTS-"--threads 1"}

bin=${SERVER_BIN:-$root/.local/build/bend-epoll}
n=$WORKERS
[[ $n =~ ^[1-9][0-9]*$ ]] || { echo "start.sh: WORKERS must be a positive integer" >&2; exit 2; }

for port in "${PORT:-3000}" "${EXTRA_PORT:-0}"; do
	[[ $port != 0 ]] || continue
	if timeout 2 bash -c "exec 3<> /dev/tcp/127.0.0.1/$port" 2> /dev/null; then
		echo "start.sh: port $port already has a listener that is not one of these workers; stop it first" >&2
		exit 2
	fi
done

pids=()
stopping=0
# stop: SIGTERM to every worker. kill gets a worker's job number, read from bash's own table
# when the signal is sent, and never its process id. Bash reaps a worker as soon as it ends,
# and from then on the number that was its process id can be another process's. For a job
# number bash looks the process up in its table with SIGCHLD blocked and sends nothing to a
# process it has reaped (bash 5.2, jobs.c, kill_pid: "avoid pid recycling problem"). A number
# that names no job any more is refused, and 2> hides that line.
stop() {
	local line
	stopping=1
	trap - TERM INT
	while read -r line; do
		if [[ $line =~ ^\[([0-9]+)\] ]]; then
			kill -TERM "%${BASH_REMATCH[1]}" 2> /dev/null || true
		fi
	done < <(jobs)
}
trap stop TERM INT
for ((k = 0; k < n && stopping == 0; k += 1)); do
	"$bin" --port "${PORT:-3000}" --extra-port "${EXTRA_PORT:-0}" --db "$DB" --pool "$POOL" \
		--sock-opts "$SOCK_OPTS" --window-us "$WINDOW_US" --gzip "$BM_GZIP" --rows "$BM_ROWS" --ring "$BM_RING" \
		--worker "$k" --workers "$n" "$@" &
	pids+=($!)
done

# The first worker to exit ends the service (the ports would be served by fewer workers
# otherwise), and its status is the script's; the rest get SIGTERM. After a SIGTERM or SIGINT to
# this script the status is 0.
rc=0
wait -n "${pids[@]}" || rc=$?
((stopping == 0)) || rc=0
stop
wait "${pids[@]}" 2>/dev/null || true
exit "$rc"
