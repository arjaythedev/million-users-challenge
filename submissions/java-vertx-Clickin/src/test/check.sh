#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
bash src/test/start-check.sh
java --enable-native-access=ALL-UNNAMED -cp target/test-classes:target/feed.jar challenge.CoreCheck
java -cp target/test-classes:target/feed.jar challenge.JsonJacksonCheck
if [[ $(uname -s) == Linux ]]; then
  output=$(java -Dio.netty.transport.noNative=true -cp target/feed.jar challenge.Application 2>&1) && {
    echo 'Application started without epoll' >&2; exit 1;
  }
  [[ $output == *'Epoll transport unavailable'* && $output == *'Caused by:'* ]] || {
    printf 'Missing epoll failure cause:\n%s\n' "$output" >&2; exit 1;
  }
  echo 'Epoll failure check passed'
fi
