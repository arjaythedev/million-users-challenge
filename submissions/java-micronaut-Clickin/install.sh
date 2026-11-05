#!/usr/bin/env bash
set -euo pipefail
if [[ $(uname -s) != Linux ]]; then
  echo 'Install JDK 26 on this OS; install.sh targets Ubuntu 24.04.' >&2
  exit 1
fi
apt-get update
apt-get install -y curl ca-certificates tar unzip zip
export SDKMAN_DIR=/opt/sdkman
if [[ ! -s "$SDKMAN_DIR/bin/sdkman-init.sh" ]]; then
  temp=$(mktemp -d)
  trap 'rm -rf "$temp"' EXIT
  curl -fsSL 'https://get.sdkman.io?ci=true&rcupdate=false' -o "$temp/sdkman-install.sh"
  bash "$temp/sdkman-install.sh"
fi
# SDKMAN's shell functions expect unset optional variables.
set +u
source "$SDKMAN_DIR/bin/sdkman-init.sh"
sdkman_auto_answer=true
sdk install java '26.0.2.1+1-tem'
sdk default java '26.0.2.1+1-tem'
set -u
java_home="$SDKMAN_DIR/candidates/java/26.0.2.1+1-tem"
update-alternatives --install /usr/bin/java java "$java_home/bin/java" 2602
update-alternatives --install /usr/bin/javac javac "$java_home/bin/javac" 2602
update-alternatives --set java "$java_home/bin/java"
update-alternatives --set javac "$java_home/bin/javac"
java -version
