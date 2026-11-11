#!/usr/bin/env bash
# Builds the server into .local/build/bend-epoll, laid out by a profile this build takes itself.
# assemble.sh puts this file at the top of a submission folder as build.sh. No profile is in
# the folder: the build takes one on the machine it runs on, in four steps.
#
#   1. compile.sh with BM_PGO_GENERATE=1: the server with clang's counters in it
#      (-fprofile-generate), moved to .local/build/pgo/bend-epoll-gen.
#   2. pgo/train.c, compiled and run: it starts that server through start.sh against a
#      PostgreSQL stand-in of its own, sends it the challenge's request mix for 8 seconds on
#      127.0.0.1 and stops it. Each worker writes its counters to .local/build/pgo/raw/ as it exits.
#   3. llvm-profdata-N merge: the workers' counters into .local/build/pgo/bend-epoll.profdata.
#   4. compile.sh with BM_PGO_PROFILE on that file: the server again (-fprofile-use), which is
#      the binary start.sh runs.
#
# compile.sh is one compile of the server: bend writes the C, clang compiles it. It is the
# build script of the repository this folder comes from, unchanged, and its header documents
# the two switches. N is the major version of the clang on the path: clang-N compiles all of
# it and llvm-profdata-N merges (install.sh installs both, and the profile runtime).
#
# The profile can only help. When a step from 1 to 3 fails, when the training does not end
# within BM_PGO_LIMIT_S seconds (60), or when clang refuses the merged file, this script writes
# one line to stderr, "build.sh: built without the profile: WHY", and builds as BM_PGO=off
# does. The exit status is then the plain build's: 0 when the server is built.
#
#   BM_PGO=off          one compile, no training: the build this folder had before
#   BM_PGO_LIMIT_S      the training's limit in whole seconds (60). It takes about 10
#   BM_PGO_TRAIN_ARGS   words added to the trainer's command line: --seconds, --conns,
#                       --workers, --min-requests, --port, --canned (pgo/train.c's header)
# Of the BM_ names these three are all this build reads. It sets compile.sh's variables itself
# and unsets every other BM_ name, CC and PROF, so that none of those, exported in the shell
# that runs the build, reaches compile.sh. What clang, pkg-config and bend read by themselves
# (CPATH, LIBRARY_PATH, PKG_CONFIG_PATH, HOME and the like) stays the caller's, as in any build.
#
# The trained binary differs from build to build: the counters depend on how the kernel
# scheduled the training. What stays the same is in the lines this script prints: the
# trainer's ("train: N requests in S s ..."), the start line of one training worker (its
# flags), the number of functions the profile has counts for, the clang command of step 4,
# and the seconds each step took.
#
# Nothing this script started is running when it exits, and it finds no process by a search:
# it reads no process table, and the one signal it sends goes to a process group it made.
#   The training runs in a process group of its own. The group's leader is a subshell of this
#   script, the guard (guard below). It starts the trainer and then waits, so the group has a
#   living leader for as long as this script wants the group. The guard's stdin is a pipe from
#   this script. That pipe ends when this script closes it (the training is over, or a
#   SIGTERM, SIGINT or SIGHUP stops the build), and when this script ends in any way at all.
#   Then the guard sends SIGKILL to its own group, by its own process id: to the trainer, the
#   start.sh that the trainer started, and the workers, whatever became of each of them.
#   This script then reads the group's other pipe to its end. Every process of the training
#   holds that pipe, so its end says that the last of them is gone (end_training below).
#   A compile (steps 1 and 4, and the build without a profile) is a foreground command. Bash
#   runs a trap when the foreground command has ended, so a SIGTERM, SIGINT or SIGHUP during
#   a compile ends this script, with status 143, when that compile is over.
# What this script cannot do, a SIGKILL to itself or to the guard, is in README.md ("When
# SIGKILL ends build.sh"). Its files go under .local/build/, with one exception: clang writes
# its temporary object files in /tmp and removes them. mkdir -p and clang's profile runtime
# also call mkdir on the directories above the one they want, which exist (README.md, "What
# the training touches").
set -uo pipefail
for v in $(compgen -v BM_) CC PROF; do
	[[ $v == BM_PGO || $v == BM_PGO_LIMIT_S || $v == BM_PGO_TRAIN_ARGS ]] || unset "$v"
done
root=$(cd "$(dirname "$0")" && pwd -P)
out=$root/.local/build/bend-epoll
work=$root/.local/build/pgo
gen=$work/bend-epoll-gen
limit=${BM_PGO_LIMIT_S:-60}
[[ $limit =~ ^[1-9][0-9]*$ ]] || limit=60
why='' training='' gpid='' status=''

# end_group ID: SIGKILL to the process group ID, and to nothing when ID is not a whole number
# above 1 (kill -- -1 is every process the user may signal, kill -- -0 the caller's own
# group). The one caller is the guard, with its own process id. A group with that id is the
# guard's own: the kernel gives a process no id that a process group still uses. When the
# guard leads no group, kill finds no such group and signals nothing. This is the only kill
# in this script.
end_group() {
	[[ ${1-} =~ ^[1-9][0-9]*$ && $1 != 1 ]] || return 2
	kill -KILL -- "-$1"
}

# guard ARG...: the leader of the training's process group, a coprocess of this script. It
# starts the trainer with ARG... and writes the trainer's exit status, as timeout gives it,
# to its stdout, which is a pipe to this script. The trainer holds that pipe too, as
# descriptor 9, and so does everything the trainer starts. Then the guard reads its stdin, on
# which this script writes nothing, to the end, and kills its group. A SIGTERM, SIGINT or
# SIGHUP to the guard does the same. timeout runs with --foreground, so it makes no group of
# its own and signals the trainer alone: SIGTERM at the limit, SIGKILL 10 s later. The line
# bash writes when a signal ends timeout goes to /dev/null.
guard() {
	set +m
	trap 'end_group "$BASHPID"; exit 1' TERM INT HUP
	{
		LLVM_PROFILE_FILE="$work/raw/%p.profraw" timeout --foreground -k 10 "$limit" "$work/train" \
			--start "$root/start.sh" --server "$gen" --log "$work/server.log" "$@" \
			9>&1 > "$work/train.out" 2> "$work/train.err" < /dev/null
		echo $?
	} 2> /dev/null &
	read -r _
	end_group "$BASHPID"
}

# end_training: ends the training's process group and returns when no process of it is left.
# It closes the guard's stdin, reads the status pipe to its end, and waits for the guard. A
# read waits 10 s at most: a process that has left the group holds the pipe until it ends, as
# the timeout of start.sh's port check does for a moment. The ids come from the coprocess's
# own variables when a signal arrived before this script copied them. It can run twice (a
# signal during it starts it again): each step is harmless the second time. 2>: the line bash
# writes about a job that a signal ended, and its word about a descriptor closed before.
end_training() {
	local from=${status:-${TRAINING[0]-}} leader=${gpid:-${TRAINING_PID-}} r=0
	[[ $training == on ]] || return 0
	# shellcheck disable=SC1083  # {NAME[1]}>&- closes the descriptor whose number is in NAME[1]
	{ [[ -z ${TRAINING[1]-} ]] || exec {TRAINING[1]}>&-; } 2> /dev/null
	while ((r == 0)) && [[ -n $from ]]; do
		read -r -t 10 -u "$from" _ 2> /dev/null
		r=$?
	done
	((r <= 1)) || echo "build.sh: a process of the training still held the build's pipe 10 s after its group was killed" >&2
	[[ -z $leader ]] || wait "$leader" 2> /dev/null
	{ [[ -z $status ]] || exec {status}<&-; } 2> /dev/null
	training=over
}
stopped() {
	trap '' TERM INT HUP
	end_training
	exit 143
}
trap stopped TERM INT HUP

# the build without a profile; its status is this script's
plain() {
	[[ -z ${1:-} ]] || echo "build.sh: built without the profile: $1" >&2
	"$root/compile.sh"
	exit $?
}
[[ ${BM_PGO:-on} != off ]] || plain

# step NAME COMMAND...: runs it with its output in .local/build/pgo/NAME.log; sets took
step() {
	local name=$1 t0=$SECONDS rc
	shift
	"$@" > "$work/$name.log" 2>&1
	rc=$?
	took=$((SECONDS - t0))
	return $rc
}

# trained: steps 1 to 4. Returns 0 with the server built from the profile, 1 with why set
# and nothing built, 2 with why set and the server built without the profile by compile.sh.
trained() {
	local v cc profdata rc funcs t1 t2 t3 t4
	v=$(clang -dumpversion 2> /dev/null | cut -d. -f1)
	cc=clang-$v profdata=llvm-profdata-$v
	command -v "$cc" > /dev/null || { why="there is no $cc beside clang"; return 1; }
	command -v "$profdata" > /dev/null || { why="$profdata is not installed"; return 1; }
	command -v timeout > /dev/null || { why="timeout is not installed"; return 1; }
	rm -rf "$work"
	mkdir -p "$work/raw" || { why="cannot write $work"; return 1; }

	CC=$cc BM_PGO_GENERATE=1 step generate "$root/compile.sh" && mv "$out" "$gen" \
		|| { why="the instrumented build failed ($(tail -n 1 "$work/generate.log"))"; return 1; }
	t1=$took

	local crypto=()
	read -r -a crypto <<< "$(pkg-config --cflags --libs libcrypto 2> /dev/null || echo -lcrypto)"
	step train-build "$cc" -std=c11 -O2 "$root/pgo/train.c" -o "$work/train" "${crypto[@]}" \
		|| { why="pgo/train.c did not compile ($(tail -n 1 "$work/train-build.log"))"; return 1; }
	local args=() t0=$SECONDS
	read -r -a args <<< "${BM_PGO_TRAIN_ARGS:-}"
	# set -m, for this one command: bash starts the coprocess as the leader of a process
	# group of its own. The copy of the status pipe is this script's own, because bash closes
	# the coprocess's two descriptors itself once the coprocess has ended.
	training=on rc=''
	set -m
	coproc TRAINING { guard "${args[@]}"; }
	set +m
	gpid=${TRAINING_PID-}
	if [[ -n $gpid ]] && exec {status}<&"${TRAINING[0]-}"; then
		read -r -u "$status" rc   # the trainer's exit status; a trapped signal ends the read
	fi
	end_training
	t2=$((SECONDS - t0))
	case $rc in
		0) ;;
		124) why="the training did not end within $limit s"; return 1 ;;
		137)
			# SIGKILL: timeout's own, when its SIGTERM at the limit did not end the trainer
			# within 10 s, or somebody else's before the limit
			if ((t2 >= limit)); then
				why="the training did not end within $limit s and had to be killed"
			else
				why="the training was killed by a SIGKILL from outside after $t2 s"
			fi
			return 1
			;;
		'') why="the training ended without a status after $t2 s"; return 1 ;;
		*) why="the training failed (exit $rc: $(tail -n 1 "$work/train.err"))"; return 1 ;;
	esac

	step merge "$profdata" merge -o "$work/bend-epoll.profdata" "$work"/raw/*.profraw \
		|| { why="$profdata merge failed ($(tail -n 1 "$work/merge.log"))"; return 1; }
	t3=$took
	# "Number of functions with maximum count (>= 1): N" and "Total functions: M"
	funcs=$("$profdata" show --value-cutoff=1 "$work/bend-epoll.profdata" \
		| awk -F': ' '/^Total functions/ { t = $2 } /maximum count \(>= 1\)/ { n = $2 } END { print n + 0 " of " t + 0 }')

	CC=$cc BM_PGO_PROFILE=$work/bend-epoll.profdata step use bash -x "$root/compile.sh" \
		|| { why="the build with the profile failed ($(grep -v '^+' "$work/use.log" | tail -n 1))"; return 1; }
	t4=$took
	if grep -q '^build.sh: .*building without the profile' "$work/use.log"; then
		why="$cc refused the profile ($(grep -m 1 'error:' "$work/use.log"))"
		return 2
	fi
	# a function whose counters do not fit the code is built without them, and clang says so
	grep 'warning: .*profile' "$work/use.log" >&2
	cat "$work/train.out"
	echo "build.sh: trained: $(grep -m 1 '^bend-epoll: worker ' "$work/server.log")"
	echo "build.sh: profile: $funcs functions have counts, from $(find "$work/raw" -name '*.profraw' | wc -l) workers ($profdata merge)"
	echo "build.sh: $(grep -m 1 -- "^+ $cc .*-fprofile-use=" "$work/use.log")"
	echo "build.sh: built $out with the profile: instrumented build $t1 s, training $t2 s, merge $t3 s, build $t4 s"
	return 0
}

trained
case $? in
	0) exit 0 ;;
	2)
		echo "build.sh: built without the profile: $why" >&2
		echo "built $out"
		exit 0
		;;
esac
plain "$why"
