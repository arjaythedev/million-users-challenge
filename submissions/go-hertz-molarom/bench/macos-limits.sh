#!/usr/bin/env bash
# Raise (or restore) the macOS limits that cap local load tests at ~16k connections:
# ephemeral port range, open-file limits, listen backlog, plus extra loopback IPs so
# mload can spread users over several (dst IP, dst port) tuples (~64k each).
#
#   bash macos-limits.sh apply    # saves current values, then raises them (uses sudo)
#   bash macos-limits.sh revert   # restores the saved values, removes the aliases
#   bash macos-limits.sh status   # prints current values
#
# sysctl -w does not survive a reboot, so a reboot is also a full revert.
set -euo pipefail

STATE="$(cd "$(dirname "$0")" && pwd)/.macos-limits.state"
ALIASES=(127.0.0.2 127.0.0.3 127.0.0.4 127.0.0.5 127.0.0.6 127.0.0.7 127.0.0.8)

# name=target
SETTINGS=(
  net.inet.ip.portrange.first=1024
  net.inet.ip.portrange.hifirst=1024
  kern.maxfiles=2500000
  kern.maxfilesperproc=1200000
  # Not 65535: XNU keeps the backlog in a signed 16-bit field, and Go passes somaxconn straight to
  # listen(), so 65535 makes every Go listener reset incoming connections.
  kern.ipc.somaxconn=32767
)

status() {
  for kv in "${SETTINGS[@]}"; do sysctl "${kv%%=*}"; done
  echo "lo0 addresses: $(ifconfig lo0 | awk '/inet /{print $2}' | tr '\n' ' ')"
  echo "ulimit -n (this shell): $(ulimit -n)"
}

apply() {
  if [[ -f "$STATE" ]]; then
    echo "state file exists ($STATE): already applied? run 'revert' first" >&2
    exit 1
  fi
  : >"$STATE"
  for kv in "${SETTINGS[@]}"; do
    echo "${kv%%=*}=$(sysctl -n "${kv%%=*}")" >>"$STATE"
  done
  # maxfiles before maxfilesperproc: the per-process limit can't exceed the global one.
  for kv in "${SETTINGS[@]}"; do sudo sysctl -w "$kv" >/dev/null; done
  for ip in "${ALIASES[@]}"; do
    if ! ifconfig lo0 | grep -q "inet $ip "; then
      sudo ifconfig lo0 alias "$ip" up
      echo "alias $ip" >>"$STATE"
    fi
  done
  echo "applied (previous values saved to $STATE)"
  status
  cat <<'EOF'

Note: each shell that runs the server or mload also needs a high fd limit:
  ulimit -n 1048576
Run the server with HOST=0.0.0.0 so it accepts on every loopback alias, and pass
mload -target 127.0.0.1,127.0.0.2,... -ports <port>,<extra port>.
EOF
}

revert() {
  if [[ ! -f "$STATE" ]]; then
    echo "no state file ($STATE): nothing to revert" >&2
    exit 1
  fi
  # Restore in reverse order so maxfilesperproc drops before maxfiles does.
  tail -r "$STATE" | while read -r line; do
    case "$line" in
      alias\ *) sudo ifconfig lo0 -alias "${line#alias }" ;;
      *=*) sudo sysctl -w "$line" >/dev/null ;;
    esac
  done
  rm -f "$STATE"
  echo "reverted"
  status
}

case "${1:-}" in
  apply) apply ;;
  revert) revert ;;
  status) status ;;
  *) echo "usage: $0 apply|revert|status" >&2; exit 2 ;;
esac
