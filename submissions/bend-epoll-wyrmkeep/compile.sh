#!/usr/bin/env bash
# Builds route A′'s host (host/main.bend with the C effects of host/epoll.c
# and host/pg.c, and host/pgwire.c, the PostgreSQL wire client, as a second
# unit) into .local/build/bend-epoll under the root. Native only.
#   bench/servers/bend-epoll/build.sh
#   PROF=1 bench/servers/bend-epoll/build.sh   # also .local/build/bend-epoll-prof: the same C
#                                              # with -g -fno-omit-frame-pointer, for perf only
# The root is the nearest directory, from this script's own upward, that holds host/pgwire.c:
# the repository root here, and the folder itself for the copy of this file that
# submission/assemble.sh puts at the top of a submission folder.
# Two steps: bend writes the C file, then $CC (default clang) compiles it and
# host/pgwire.c with the flags bend uses (-std=c11 -O3, vendor/bend/bend2/main.ts:508)
# plus the three libraries the effects need, which bend's own build does not link:
#   libcrypto   SHA-256 for the JWT check (host/http.h) and for SCRAM-SHA-256
#               (host/pgwire.c): pkg-config's libcrypto, else Homebrew's openssl@3
#               or openssl
#   libdeflate  pkg-config, else Homebrew's libdeflate (level 1, for --gzip libdeflate;
#               host/gzip.h, the default encoder, needs no library)
#   libisal     ISA-L, pkg-config's libisal (Ubuntu's libisal-dev), else Homebrew's isa-l
#               (igzip level 0, for --gzip isal: host/isal.h)
# No libpq: host/pgwire.c speaks the PostgreSQL protocol itself (S25).
#
# Build switches (issue #66, S29), all from the environment and all optional. With none set
# the two clang commands are the ones above, argument for argument:
#   BM_CFLAGS_EXTRA   words added after -O3 on both units and on the link, split on blanks
#                     (BM_CFLAGS_EXTRA="-march=x86-64-v3 -fno-plt")
#   BM_LTO            thin or full: -flto=thin or -flto=full over both units. The link then needs
#                     LLVMgold.so beside GNU ld (Ubuntu 24.04: llvm-N-linker-tools, which
#                     clang-N depends on) or lld (BM_CFLAGS_EXTRA=-fuse-ld=lld)
#   BM_PGO_GENERATE   1: -fprofile-generate, the instrumented build. Each process writes
#                     default_<hash>.profraw into its working directory when it exits
#                     (LLVM_PROFILE_FILE names another place); needs the profile runtime
#                     (Ubuntu 24.04: libclang-rt-N-dev). flags-run.sh train runs it and merges
#   BM_PGO_PROFILE    the indexed profile llvm-profdata merge wrote: -fprofile-use=FILE. A
#                     profile fits the clang that wrote it and the C it was written from, and
#                     it never costs the build: clang warns about every function whose profile
#                     is out of date ("hash mismatch") and builds that function without one;
#                     when clang refuses the file (a newer format: clang 18 refuses clang
#                     20's; a --text profile; a damaged file) or the file cannot be read,
#                     build.sh says so on stderr ("building without the profile") and builds
#                     as if the switch were not set
#   BM_STATIC_LIBS    1: libcrypto.a and libdeflate.a instead of the shared libraries. ISA-L
#                     stays shared: Ubuntu 24.04's libisal-dev ships no libisal.a
# Both PGO switches name a static function by its file's base name, not its path
# (-mllvm -static-func-full-module-prefix=false), so that a profile written in one checkout
# fits a build in another.
set -euo pipefail
root="$(cd "$(dirname "$0")" && pwd)"
while [[ ! -f $root/host/pgwire.c ]]; do
	[[ $root != / ]] || { echo "${0##*/}: no host/pgwire.c above $0" >&2; exit 2; }
	root="$(dirname "$root")"
done
export PATH="$HOME/.bend/bin:$PATH" BEND_NO_TELEMETRY=1
brew=/home/linuxbrew/.linuxbrew/opt
cc=${CC:-clang}
out=$root/.local/build
mkdir -p "$out"

crypto=()
if pkg-config --exists libcrypto 2>/dev/null; then
	read -r -a crypto <<< "$(pkg-config --cflags --libs libcrypto)"
else
	for d in "$brew/openssl@3" "$brew/openssl"; do
		if [[ -e $d/include/openssl/sha.h ]]; then
			crypto=(-I "$d/include" -L "$d/lib" "-Wl,-rpath,$d/lib" -lcrypto)
			break
		fi
	done
fi
((${#crypto[@]} > 0)) || { echo "build.sh: no libcrypto: install libssl-dev (openssl-devel) or brew install openssl@3" >&2; exit 1; }

deflate=()
if pkg-config --exists libdeflate 2>/dev/null; then
	read -r -a deflate <<< "$(pkg-config --cflags --libs libdeflate)"
elif [[ -e $brew/libdeflate/include/libdeflate.h ]]; then
	deflate=(-I "$brew/libdeflate/include" -L "$brew/libdeflate/lib" "-Wl,-rpath,$brew/libdeflate/lib" -ldeflate)
else
	echo "build.sh: no libdeflate: install libdeflate-dev or brew install libdeflate" >&2
	exit 1
fi

isal=()
if pkg-config --exists libisal 2>/dev/null; then
	read -r -a isal <<< "$(pkg-config --cflags --libs libisal)"
elif [[ -e $brew/isa-l/include/isa-l/igzip_lib.h ]]; then
	isal=(-I "$brew/isa-l/include" -L "$brew/isa-l/lib" "-Wl,-rpath,$brew/isa-l/lib" -lisal)
else
	echo "build.sh: no ISA-L (isa-l/igzip_lib.h): install libisal-dev or brew install isa-l" >&2
	exit 1
fi

# the switches; extra and pgo stay empty without them
extra=() pgo=()
read -r -a extra <<< "${BM_CFLAGS_EXTRA:-}"
case ${BM_LTO:-} in
	'') ;;
	thin | full) extra+=("-flto=$BM_LTO") ;;
	*) echo "build.sh: BM_LTO is thin or full, not $BM_LTO" >&2; exit 2 ;;
esac
if [[ ${BM_PGO_GENERATE:-0} == 1 && -n ${BM_PGO_PROFILE:-} ]]; then
	echo "build.sh: BM_PGO_GENERATE and BM_PGO_PROFILE are two builds: set one" >&2
	exit 2
elif [[ ${BM_PGO_GENERATE:-0} == 1 ]]; then
	extra+=(-fprofile-generate -mllvm -static-func-full-module-prefix=false)
elif [[ -n ${BM_PGO_PROFILE:-} ]]; then
	if [[ -r $BM_PGO_PROFILE ]]; then
		pgo=("-fprofile-use=$(realpath "$BM_PGO_PROFILE")" -mllvm -static-func-full-module-prefix=false)
	else
		echo "build.sh: BM_PGO_PROFILE=$BM_PGO_PROFILE is not a readable file: building without the profile" >&2
	fi
fi
if [[ ${BM_STATIC_LIBS:-0} == 1 ]]; then
	crypto=("${crypto[@]/#-lcrypto/-l:libcrypto.a}")
	deflate=("${deflate[@]/#-ldeflate/-l:libdeflate.a}")
fi

cd "$root"
bend host/main.bend -o "$out/bend-epoll.c"
# compile OUTPUT [FLAG ...]: both units into OUTPUT. A profile must never cost the build: when
# clang fails with BM_PGO_PROFILE (a profile of a newer format, a damaged file), the same command
# runs again without it and stderr says so.
compile() {
	local o=$1
	shift
	if ((${#pgo[@]} > 0)); then
		"$cc" -std=c11 -O3 "${extra[@]}" "${pgo[@]}" "$@" "$out/bend-epoll.c" "$root/host/pgwire.c" -I "$root/host" \
			"${crypto[@]}" "${deflate[@]}" "${isal[@]}" -lpthread -lm -o "$o" && return 0
		echo "build.sh: clang failed with BM_PGO_PROFILE=$BM_PGO_PROFILE: building without the profile" >&2
		pgo=()
	fi
	"$cc" -std=c11 -O3 "${extra[@]}" "$@" "$out/bend-epoll.c" "$root/host/pgwire.c" -I "$root/host" "${crypto[@]}" "${deflate[@]}" \
		"${isal[@]}" -lpthread -lm -o "$o"
}
compile "$out/bend-epoll"
echo "built $out/bend-epoll"
if [[ ${PROF:-0} == 1 ]]; then
	compile "$out/bend-epoll-prof" -g -fno-omit-frame-pointer
	echo "built $out/bend-epoll-prof"
fi
