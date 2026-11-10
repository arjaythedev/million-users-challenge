#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || -z $1 ]]; then
  printf 'Usage: %s SERVER_URL\n' "$0" >&2
  exit 2
fi
base=${1%/}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT

get() {
  local path=$1 expected=$2 status
  status=$(curl -sS --compressed --max-time 10 -o "$dir/body" -w '%{http_code}' "$base$path")
  if [[ $status != "$expected" ]]; then
    printf '%s: expected HTTP %s, got %s\n' "$path" "$expected" "$status" >&2
    cat "$dir/body" >&2
    exit 1
  fi
}

keys='["id", "body", "created_at", "author", "like_count"]'
get /health 200
jq -e '.status == "ok" and .db == "ok" and (.uptime_s | type == "number")' "$dir/body" >/dev/null
get /feed 200
jq -e --argjson keys "$keys" '.posts | length == 20 and all(.[]; keys_unsorted == $keys)' "$dir/body" >/dev/null
get /posts/1 200
jq -e --argjson keys "$keys" '.post | .id == 1 and keys_unsorted == $keys' "$dir/body" >/dev/null
get /posts/999999999 404
jq -e '.error == "post not found"' "$dir/body" >/dev/null
printf 'Database smoke checks passed for %s\n' "$base"
