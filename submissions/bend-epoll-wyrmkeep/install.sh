#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: everything build.sh needs. From apt: curl, pkg-config,
# libcrypto (libssl-dev: SHA-256 for the JWT check and SCRAM), libdeflate and ISA-L (libisal-dev),
# at the archive's versions, and clang. From github.com: bend, checked against a pinned sha256.
# Safe to run again: it replaces BEND_HOME, and apt skips a package that is present.
#   sudo ./install.sh
# Writes under BEND_HOME and /usr/local/bin, and through apt. build.sh downloads nothing.
set -euo pipefail

# bend. The archive and its layout are the ones the installer at https://bend-lang.com/install.sh
# fetches, and the sha256 is that installer's SHA_LINUX_X64 for 2.0.40. The installer itself is
# not run: it installs the release it was last written for, under the caller's HOME (root's here,
# which the build user cannot read). An installed bend asks bend-lang.com for the latest version
# once a day unless BEND_NO_TELEMETRY=1 is set. This script and build.sh set it on every call.
BEND_VERSION=2.0.40
BEND_URL=https://github.com/bendlang/bend/releases/download/v2.0.40/bend-2.0.40-linux-x64.tar.gz
BEND_SHA256=c9ff8d67bd903fb6ad9a26b43d8a62c01021de82a44d55ce4a3dcd4732113d6b
BEND_HOME=/opt/bend-2.0.40

# clang, the first of these packages that apt offers. bend's C asks for the preserve_none calling
# convention and falls back to the default one when the compiler lacks it. Of Ubuntu 24.04's
# clang packages, 19 and 20 have it (checked 2026-11-09) and 14 to 18 do not. The unversioned
# clang package is 18: the build still works with it, on the fallback.
CLANG_PACKAGES="clang-20 clang-19 clang"

[[ $(id -u) == 0 ]] || { echo "install.sh: run as root (sudo ./install.sh)" >&2; exit 2; }

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl pkg-config libssl-dev libdeflate-dev libisal-dev

# candidate PACKAGE: apt has a version of it to install
candidate() {
	local c
	c=$(apt-cache policy "$1" 2> /dev/null | awk '/Candidate:/ { print $2 }')
	[[ -n $c && $c != "(none)" ]]
}

pkg=
for p in $CLANG_PACKAGES; do
	if candidate "$p"; then
		pkg=$p
		break
	fi
done
[[ -n $pkg ]] || { echo "install.sh: apt offers none of: $CLANG_PACKAGES" >&2; exit 1; }
apt-get install -y --no-install-recommends "$pkg"
# build.sh runs "clang". A clang-N package installs only /usr/bin/clang-N, and a machine may
# have another clang on the path already: the name must run the package chosen here.
on_path=$(command -v clang || true)
if [[ -z $on_path || $(readlink -f "$on_path") != "$(readlink -f "/usr/bin/$pkg")" ]]; then
	ln -sfn "/usr/bin/$pkg" /usr/local/bin/clang
fi

# The profile build.sh takes at build time needs two more packages of the same LLVM, N being
# the clang's major version: llvm-N has llvm-profdata-N, which merges the counters the
# training run writes, and libclang-rt-N-dev has the runtime the instrumented build links.
# build.sh builds without a profile when one is missing, so apt's refusal here is a line on
# stderr and not a failure of this script. The last line is for the record of this run.
llvm=$(clang -dumpversion | cut -d. -f1)
apt-get install -y --no-install-recommends "llvm-$llvm" "libclang-rt-$llvm-dev" \
	|| echo "install.sh: apt did not install llvm-$llvm and libclang-rt-$llvm-dev: build.sh will build without a profile" >&2
echo "llvm-profdata-$llvm: $(command -v "llvm-profdata-$llvm" || echo "not installed, build.sh builds without a profile")"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl --proto '=https' --tlsv1.2 -fsSL "$BEND_URL" -o "$tmp/bend.tar.gz"
echo "$BEND_SHA256  $tmp/bend.tar.gz" | sha256sum -c - \
	|| { echo "install.sh: $BEND_URL is not the pinned archive (sha256 $BEND_SHA256)" >&2; exit 1; }
tar -xzf "$tmp/bend.tar.gz" -C "$tmp" --no-same-owner
rm -rf "$BEND_HOME"
mkdir -p "$BEND_HOME/bin"
mv "$tmp/bend/bend2" "$tmp/bend/guide" "$BEND_HOME/"
mv "$tmp/bend/bin/bend" "$BEND_HOME/bin/bend"
chmod -R a+rX "$BEND_HOME"
# bend finds bend2/ from the real path of its executable, so a link to it is enough.
ln -sfn "$BEND_HOME/bin/bend" /usr/local/bin/bend

have=$(BEND_NO_TELEMETRY=1 bend version)
[[ $have == "bend $BEND_VERSION" ]] || { echo "install.sh: bend version printed '$have', not 'bend $BEND_VERSION'" >&2; exit 1; }

# For the record: what this run installed.
echo "$have at $BEND_HOME"
clang --version | sed -n 1p
echo "libcrypto $(pkg-config --modversion libcrypto), libdeflate $(pkg-config --modversion libdeflate), libisal $(pkg-config --modversion libisal)"
