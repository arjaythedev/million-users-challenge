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
#   WORKERS    processes, default nproc, the CPUs this script may run on, and below 4 CPUs
#              the script's WORKERS_FLOOR, 4 (the form, below)
#   POOL       database connections per process, default 16, the value the lab measured (at four
#              workers). A process exits 64 when (POOL + 1) x WORKERS is over 512 (one cancel
#              connection per process): with POOL=16, above 30 workers
#   SOCK_OPTS  --sock-opts, default off
#   BM_ACK_FIRST
#              --ack-first, default off, the binary's own default: on sets TCP_QUICKACK 0 on the
#              listeners after listen(), so the ACK of a connection's first request goes in its
#              reply's segment, and sets nothing else of SOCK_OPTS (an experiment flag, S64)
#   BM_ACK_LATE
#              --ack-late, default off, the binary's own default: on sets TCP_QUICKACK 0 on a
#              client's socket before the write of a reply that leaves 25 ms or more after its
#              request arrived, so the connection's next request gets no ACK of its own, and
#              sets nothing else of SOCK_OPTS (an experiment flag, S64). SOCK_OPTS=on sets
#              both, whatever these say
#   WINDOW_US  --window-us, default 1000 on 4 CPUs and more (the form, below): after a wake that
#              brought events a worker's next turn sleeps until 1 ms after that wake. Below 4
#              CPUs the default is 2000, the binary's own and the value before S80
#   BM_WINDOW_HOLD
#              --window-hold, default off, the binary's own default: a worker skips its window
#              after a wake that filled its event array and while requests wait on its ring
#              (on: it takes the window then too, so under a backlog it lexes at most --max
#              requests a window; an experiment flag, S57)
#   DB         --db, default pg
#   BM_GZIP    --gzip, default isal: ISA-L's level 0 stream (fixed and libdeflate are the others)
#   BM_ROWS    --rows, default block: a feed's rows cross to the Bend code as one cell (each: one
#              per row)
#   BM_RING    --ring, default lifo: under a backlog a worker answers the newest request first
#              (fifo: the oldest first, the binary's own default)
#   BM_RING_OLD
#              --ring-old, default 64 on 4 CPUs and more (the form, below): in a newest-first
#              turn every 64th arrival comes from the ring's old end (1 to 1024; the binary
#              judges it, as it judges every value this script passes on unchanged). Empty,
#              --ring-old is not passed, and the binary's own 8 stands: that is the default
#              below 4 CPUs, and what a variable that is set and empty gives (S74, S80)
#   BM_READ_BATCH_MAX
#              --read-batch-max, default 16 on 4 CPUs and more (the form, below): a worker
#              flushes its open read batch at that many statements, and a turn lexes no more
#              requests than that batch has room for (1 to --batch-max; the binary judges it).
#              Empty, --read-batch-max is not passed, and the binary's own cap stands,
#              --batch-max (64): that is the default below 4 CPUs, and what a variable that is
#              set and empty gives (S79, S80)
#   BM_RESULT  --result-format, default binary, the binary's own default: PostgreSQL sends every
#              result column in binary (mixed: a feed row's id and like_count as decimal text,
#              which is fewer bytes a feed)
#   BM_PICK    --pick, default spread, the binary's own default: a flushed batch goes to the
#              ready connection of its kind with the fewest batches in flight (fill: to the first
#              with fewer than BM_FILL_DEPTH in flight, so that a connection which waits for a
#              reply gets the next batch too; fill-reads: fill for a read batch, spread for a
#              write batch)
#   BM_FILL_DEPTH
#              --fill-depth, default 2, the binary's own default (1 to 8): read under --pick fill
#              and fill-reads. BM_GZIP has a prefix because gzip(1) reads a variable named GZIP;
#              the others follow it
#   BM_DEPTH_MAX
#              --depth-max, default 0, the binary's own default: no bound (D, 1 to 64: a read
#              batch is not sent to a database connection that carries D batches; it waits for
#              one to answer, and the worker serves its client sockets meanwhile; an experiment
#              flag, S63)
#   BM_BUSY_MAX
#              --busy-max for every worker as it is, default 0: the bound then comes from
#              BM_BUSY_ALL (N, 1 to 1000000: a worker lexes no request while N of its requests
#              have no reply yet, and serves its client sockets meanwhile; S63). The two are two
#              forms of one bound: with both above 0 every worker stops at its start with a line
#              saying so
#   BM_BUSY_ALL
#              a count for the whole server (0 to 1000000, 0: no bound), default 2000 on 4
#              CPUs and more (the form, below) and 0 below 4 CPUs: every
#              worker gets --busy-max with the count divided by WORKERS, rounded up, so the
#              workers' bounds add up to the count, or to at most WORKERS - 1 more (2000 is 1000
#              a worker on 2 workers, 500 on 4, 250 on 8, 125 on 16 and 667 on 3). The workers share one
#              database, so the count that keeps the bound's time at any worker count is the
#              whole server's. The default is for a caller that sets no bound of its own: with
#              BM_BUSY_MAX or BM_DEPTH_MAX not 0 it is 0, so that bound starts alone, as before
#              S80. Set and empty is 0 too. Before it starts any worker the script refuses
#              (exit 2) a value that is not a whole number from 0 to 1000000, and a value above
#              0 beside a BM_BUSY_MAX that is not 0: the two variables set one flag. With
#              BM_DEPTH_MAX above 0 beside a count that the caller set, the workers stop at
#              their start, as for BM_BUSY_MAX (S74; the default since S80)
#   BENDRTS    the Bend runtime's flags, default --threads 1 (set, even empty, it stays): the server
#              makes no parallel call, and with one thread the evaluator keeps its continuations on
#              the stack rather than the heap (the repository's bench/servers/bend-epoll/README.md, Workers)
# The form (S80). On a machine of 4 CPUs or more, as nproc counts them, and with no variable
# set, the workers start with four values that are not the binary's own: WINDOW_US 1000,
# BM_RING_OLD 64, BM_BUSY_ALL 2000 and BM_READ_BATCH_MAX 16. Below 4 CPUs the four are what
# they were before S80: the 2 ms window, no --ring-old, no bound and no --read-batch-max. The
# form is for bursts on a machine with CPU time to spare. On 2 CPUs the CPU time was short
# for the whole of a lab run, and the form failed a count of users that the values before
# S80 passed, one row each (the repository's bench/servers/bend-epoll/README.md, What the
# start values assume). The rule reads the machine and not WORKERS, and a variable that the
# caller sets wins on every machine. One line sets the four back on every machine:
#   BM_BUSY_ALL=0 BM_RING_OLD= WINDOW_US=2000 BM_READ_BATCH_MAX= start.sh
# Every worker then gets the arguments it got before S80 with no variable set, word for word.
# Below 4 CPUs the script also starts WORKERS_FLOOR processes where it started one a CPU,
# when WORKERS is not set: in the lab's two rows with 2 CPUs and these values, 4 processes
# left the CPU that takes the interrupts less busy than 2 did, and fewer replies were late
# (the same part of the README). WORKERS=$(nproc) beside the line above gives the count
# before S80 there too.
# Arguments go to every worker, after the flags this script passes, so an argument wins over a
# default (lab-run.sh: --batch-wait US --stats FILE; each worker writes FILE.K, and --trace FILE
# likewise). Where the script starts the form, an argument --depth-max above 0 meets the
# default bound, and the workers stop at their start: set BM_BUSY_ALL=0 beside it, or give
# the depth as BM_DEPTH_MAX.
# An argument --batch-max below 16 meets the default cap the same way: set BM_READ_BATCH_MAX=
# beside it, or pass a --read-batch-max.
# Foreground. SIGTERM or SIGINT goes to every worker. When one worker exits, the others
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
cpus=$(nproc)                  # the number of processing units available to this script
[[ $cpus =~ ^[1-9][0-9]*$ ]] || { echo "start.sh: nproc printed \"$cpus\", which is not a count of CPUs" >&2; exit 2; }
# The form's four values (S80), and the one place that says where the script starts the form.
form_all=2000     # BM_BUSY_ALL, when the caller sets no bound of its own
form_old=64       # BM_RING_OLD
form_window=1000  # WINDOW_US
form_cap=16       # BM_READ_BATCH_MAX: the one line that holds the cap's default
form_workers=$cpus  # WORKERS: one process a CPU
WORKERS_FLOOR=4   # the processes below 4 CPUs, when WORKERS is not set
# Below 4 CPUs the script does not start the form: the four are what they were before S80,
# and the processes are WORKERS_FLOOR
if [[ $cpus =~ ^[123]$ ]]; then
	form_all=0 form_old='' form_window=2000 form_cap=''
	form_workers=$WORKERS_FLOOR
fi
WORKERS=${WORKERS:-$form_workers}  # processes
POOL=${POOL:-16}               # database connections per process
SOCK_OPTS=${SOCK_OPTS:-off}    # --sock-opts on|off
BM_ACK_FIRST=${BM_ACK_FIRST:-off}  # --ack-first on|off
BM_ACK_LATE=${BM_ACK_LATE:-off}    # --ack-late on|off
WINDOW_US=${WINDOW_US:-$form_window}  # --window-us
BM_WINDOW_HOLD=${BM_WINDOW_HOLD:-off}  # --window-hold on|off
DB=${DB:-pg}                   # --db
BM_GZIP=${BM_GZIP:-isal}       # --gzip isal|fixed|libdeflate
BM_ROWS=${BM_ROWS:-block}      # --rows block|each
BM_RING=${BM_RING:-lifo}       # --ring lifo|fifo
BM_RESULT=${BM_RESULT:-binary} # --result-format binary|mixed
BM_PICK=${BM_PICK:-spread}     # --pick spread|fill|fill-reads
BM_FILL_DEPTH=${BM_FILL_DEPTH:-2}  # --fill-depth 1..8, read under --pick fill and fill-reads
BM_DEPTH_MAX=${BM_DEPTH_MAX:-0}  # --depth-max 0..64, 0 is no bound
BM_BUSY_MAX=${BM_BUSY_MAX:-0}    # --busy-max 0..1000000, 0 is no bound
# BM_BUSY_ALL: 0..1000000 for the whole server, 0 is no bound; above 0 it sets --busy-max. The
# default is for a caller that sets no bound of its own. With BM_BUSY_MAX or BM_DEPTH_MAX not 0
# the variable gets no default here, and the line after the test makes it 0, as it makes a
# variable that is set and empty.
if [[ $BM_BUSY_MAX =~ ^0+$ && $BM_DEPTH_MAX =~ ^0+$ ]]; then
	BM_BUSY_ALL=${BM_BUSY_ALL-$form_all}
fi
BM_BUSY_ALL=${BM_BUSY_ALL:-0}
BM_RING_OLD=${BM_RING_OLD-$form_old}  # --ring-old 1..1024; empty: not passed
BM_READ_BATCH_MAX=${BM_READ_BATCH_MAX-$form_cap}  # --read-batch-max 1..--batch-max; empty: not passed
export BENDRTS=${BENDRTS-"--threads 1"}

bin=${SERVER_BIN:-$root/.local/build/bend-epoll}
n=$WORKERS
[[ $n =~ ^[1-9][0-9]*$ ]] || { echo "start.sh: WORKERS must be a positive integer" >&2; exit 2; }

# BM_BUSY_ALL is the one value this script computes with, so the script reads it, the way the
# binary reads a count (ep_num in host/epoll.c): digits only, base 10. 0500 is 500, never
# bash's octal, and at most 7 digits reach the arithmetic.
if [[ $BM_BUSY_ALL =~ ^0*([0-9]{1,7})$ ]] && ((10#${BASH_REMATCH[1]} <= 1000000)); then
	all=$((10#${BASH_REMATCH[1]}))
else
	printf 'start.sh: BM_BUSY_ALL must be a whole number from 0 to 1000000, not %q\n' "$BM_BUSY_ALL" >&2
	exit 2
fi
# busy: what every worker gets as --busy-max. BM_BUSY_MAX as it is, or the worker's share of
# BM_BUSY_ALL. The share rounds up, so a count above 0 never becomes 0, which is no bound.
busy=$BM_BUSY_MAX
if ((all > 0)); then
	if [[ ! $BM_BUSY_MAX =~ ^0+$ ]]; then
		printf 'start.sh: BM_BUSY_ALL=%s and BM_BUSY_MAX=%q both set --busy-max: set one of them\n' "$BM_BUSY_ALL" "$BM_BUSY_MAX" >&2
		exit 2
	fi
	busy=$(((all + n - 1) / n))
fi
ring_old=()
[[ -z $BM_RING_OLD ]] || ring_old=(--ring-old "$BM_RING_OLD")
read_batch_max=()
[[ -z $BM_READ_BATCH_MAX ]] || read_batch_max=(--read-batch-max "$BM_READ_BATCH_MAX")

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
	"$bin" --port "${PORT:-3000}" --extra-port "${EXTRA_PORT:-0}" --db "$DB" --pool "$POOL" "${read_batch_max[@]}" \
		--sock-opts "$SOCK_OPTS" --window-us "$WINDOW_US" --window-hold "$BM_WINDOW_HOLD" \
		--gzip "$BM_GZIP" --rows "$BM_ROWS" --ring "$BM_RING" "${ring_old[@]}" \
		--result-format "$BM_RESULT" --pick "$BM_PICK" --fill-depth "$BM_FILL_DEPTH" \
		--depth-max "$BM_DEPTH_MAX" --busy-max "$busy" \
		--ack-first "$BM_ACK_FIRST" --ack-late "$BM_ACK_LATE" \
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
