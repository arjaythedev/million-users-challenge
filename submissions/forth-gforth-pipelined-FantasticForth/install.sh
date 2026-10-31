#!/usr/bin/env bash
# install.sh: runs once as root on a clean Ubuntu 24.04.
# Installs the gforth toolchain and the C libraries the FFI layer links against.
#
# Versions are pinned to the Ubuntu 24.04 (Noble) archive -- the distro release
# IS the pin, and nothing external is downloaded or built. The builds this was
# developed and tested against:
#   gforth          0.7.3+dfsg-9build4.1
#   libpq-dev       16.x-0ubuntu0.24.04.x  (PostgreSQL client library, libpq)
#   libssl-dev      3.0.13-0ubuntu3.x      (OpenSSL 3.0.13)
#   libtool-bin     2.4.7-7build1
#   build-essential 12.10ubuntu1
# We intentionally let apt resolve the exact point build rather than hard-pinning
# the "-Nubuntu" string: Ubuntu security updates supersede and purge the older
# point build, so a hard `pkg=exact-version` could fail to install on the box.
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
apt-get update
# gforth           : the Forth system (provides the c-library FFI used for libpq/crypto/sockets)
# build-essential  : cc, needed by gforth's c-library to compile FFI glue at load time
# libtool-bin      : provides the `libtool` *binary* gforth's c-library invokes
#                    (the plain `libtool` package ships only m4 macros, not the command)
# libpq-dev        : provides -lpq (libpq.so symlink) + libpq-fe.h for the FFI link step.
#                    On Ubuntu the header lands in /usr/include/postgresql/ (ffi.fs
#                    includes it by that absolute path; asserted in build.sh).
# libssl-dev       : provides -lcrypto (libcrypto.so symlink) for HMAC-SHA256 (JWT)
# postgresql-client, ca-certs : psql for local testing / convenience, TLS roots
apt-get install -y --no-install-recommends \
  gforth \
  build-essential \
  libtool-bin \
  libpq-dev \
  libssl-dev \
  postgresql-client \
  ca-certificates

# Show versions for the build log.
gforth --version 2>&1 | head -1 || true
dpkg -s libpq-dev 2>/dev/null | grep -i '^Version' || true
echo "install.sh complete"
