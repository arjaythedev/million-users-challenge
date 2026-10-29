# amd64 Ubuntu 24.04 with the aarch64 cross toolchain (the default gcc), Kitware's CMake and arm64
# copies of the libraries install.sh pulls in. arm64 binaries (the unit tests) run through the host's qemu binfmt handler.
FROM --platform=linux/amd64 ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN sed -i '/^Components:/a Architectures: amd64' /etc/apt/sources.list.d/ubuntu.sources && \
    printf '%s\n' \
      'Types: deb' \
      'URIs: http://ports.ubuntu.com/ubuntu-ports' \
      'Suites: noble noble-updates noble-security' \
      'Components: main universe' \
      'Architectures: arm64' \
      'Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg' \
      > /etc/apt/sources.list.d/ubuntu-ports.sources && \
    dpkg --add-architecture arm64 && \
    apt-get update && \
    apt-get install -y --no-install-recommends software-properties-common curl gpg ca-certificates && \
    add-apt-repository -y ppa:ubuntu-toolchain-r/test && \
    curl -fsSL https://apt.kitware.com/keys/kitware-archive-latest.asc | gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg && \
    echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ noble main' \
      > /etc/apt/sources.list.d/kitware.list && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
      binutils-aarch64-linux-gnu gcc-aarch64-linux-gnu g++-aarch64-linux-gnu cmake ninja-build git make file \
      libc6:arm64 libstdc++6:arm64 liburing-dev:arm64 && \
    rm -rf /var/lib/apt/lists/*
