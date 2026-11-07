#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04. Idempotent. Installs only Ubuntu packages and adds no
# apt sources, so the box's shared libraries (its libpq included) stay as they were.
#  - python3 (3.12), venv, headers and a C compiler: the C extensions are built on the box.
#  - make: build.sh compiles libpq 18 from the PostgreSQL source tarball (Ubuntu's libpq is 16;
#    libpq 17+ has PQsendPipelineSync, which marks a sync point *without* flushing, so every query a
#    worker issues during one event-loop tick leaves in a single write(2)), and Intel ISA-L.
#  - bison, flex and perl: PostgreSQL's build requires them (perl generates libpq's headers;
#    the tarball's pre-generated parsers are used).
#  - nasm: ISA-L's igzip (the response compressor) has x86-64 kernels in assembly.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  python3 python3-venv python3-dev gcc libc6-dev make bison flex perl nasm ca-certificates
python3 --version
