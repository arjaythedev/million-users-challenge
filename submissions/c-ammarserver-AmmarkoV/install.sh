#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04.
#   build-essential cmake git  : AmmarServer is built from source with its own CMake
#   libssl-dev                 : libcrypto, for the HMAC-SHA256 behind every token verification
#   libpq-dev                  : libpq, the Postgres client library ( 16.x on 24.04 , which talks to Postgres 18 )
#   libdeflate-dev             : gzip for the feed responses
#   ca-certificates            : fetching the pinned AmmarServer commit over https in build.sh
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    libssl-dev \
    libpq-dev \
    libdeflate-dev \
    ca-certificates
