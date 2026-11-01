#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04. Idempotent.
#  - python3 (3.12), venv, headers and a C compiler: the libpq adapter is a C extension built on the box.
#  - libpq from the PostgreSQL project's own apt repository (apt.postgresql.org), not Ubuntu's 16.x:
#    libpq 17+ has PQsendPipelineSync, which marks a sync point *without* flushing, so every query a
#    worker issues during one event-loop tick leaves in a single write(2). The client library then
#    also matches the Postgres 18 server.
#  - nasm and make: build.sh compiles Intel ISA-L (igzip, the response compressor), whose x86-64
#    kernels are assembly.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  python3 python3-venv python3-dev gcc libc6-dev make nasm pkg-config ca-certificates postgresql-common
/usr/share/postgresql-common/pgdg/apt.postgresql.org.sh -y
apt-get install -y --no-install-recommends libpq-dev libpq5
python3 --version
pkg-config --modversion libpq
