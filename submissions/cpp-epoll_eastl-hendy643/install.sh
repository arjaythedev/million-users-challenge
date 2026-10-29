#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends curl gpg ca-certificates
# Newest CMake that supports Ubuntu 24.04: Kitware's apt repository.
curl -fsSL https://apt.kitware.com/keys/kitware-archive-latest.asc | gpg --dearmor --yes -o /usr/share/keyrings/kitware-archive-keyring.gpg
echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ noble main' \
  > /etc/apt/sources.list.d/kitware.list
apt-get update
# clang-20 lld-20 llvm-20 libclang-rt-20-dev: the compiler build.sh uses (llvm supplies llvm-profdata, the runtime the
# profiling hooks). gcc g++: the other one (FEED_COMPILER=gcc).
# git: CPM.cmake fetches the other dependencies with it.
# liburing-dev: the distribution's io_uring library. Every other library is built from source and linked statically.
apt-get install -y --no-install-recommends gcc g++ clang-20 lld-20 llvm-20 libclang-rt-20-dev cmake ninja-build git liburing-dev
g++ --version
clang++-20 --version
cmake --version
