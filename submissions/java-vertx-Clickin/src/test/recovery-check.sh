#!/usr/bin/env bash
# RECOVERY_DATABASE_URL must point at localhost:15432, a temporary proxy to PostgreSQL.
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${RECOVERY_DATABASE_URL:?Set RECOVERY_DATABASE_URL to a test database at localhost:15432}"
dir=$(mktemp -d)
port=${PORT:-13302}
extra=${EXTRA_PORT:-13303}
export DATABASE_URL=$RECOVERY_DATABASE_URL HOST=127.0.0.1 PORT=$port EXTRA_PORT=$extra MU_EVENT_LOOPS=1 MU_DB_POOL=1 MU_GZIP_LEVEL=0
export MU_HEAP=${MU_HEAP:-512m}
bash start.sh > "$dir/server.log" 2>&1 &
pid=$!
proxy=
cleanup() {
  [[ -z $proxy ]] || { kill "$proxy" 2>/dev/null || true; wait "$proxy" 2>/dev/null || true; }
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  rm -rf "$dir"
}
trap cleanup EXIT
status=000
for _ in $(seq 1 30); do
  status=$(curl -s --max-time 8 -o "$dir/body" -w '%{http_code}' "http://127.0.0.1:$port/health" || true)
  [[ $status == 503 ]] && break
  if ! kill -0 "$pid" 2>/dev/null; then cat "$dir/server.log" >&2; exit 1; fi
  sleep 1
done
[[ $status == 503 && $(cat "$dir/body") == '{"status":"degraded","db":"unreachable","error":"internal server error"}' ]] || {
  printf 'Expected a degraded health response, got %s\n' "$status" >&2; cat "$dir/server.log" >&2; exit 1;
}
cat > "$dir/proxy.py" <<'PY'
import select
import socket
import socketserver

class Proxy(socketserver.BaseRequestHandler):
    def handle(self):
        with socket.create_connection(("127.0.0.1", 5432), timeout=5) as upstream:
            upstream.settimeout(None)
            peers = {self.request: upstream, upstream: self.request}
            while True:
                readable, _, _ = select.select(list(peers), [], [], 30)
                for source in readable:
                    data = source.recv(65536)
                    if not data:
                        return
                    peers[source].sendall(data)

class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

with Server(("127.0.0.1", 15432), Proxy) as server:
    server.serve_forever()
PY
python3 "$dir/proxy.py" > "$dir/proxy.log" 2>&1 &
proxy=$!
status=000
for _ in $(seq 1 30); do
  status=$(curl -s --max-time 8 -o "$dir/body" -w '%{http_code}' "http://127.0.0.1:$port/health" || true)
  [[ $status == 200 ]] && break
  if ! kill -0 "$proxy" 2>/dev/null; then cat "$dir/proxy.log" >&2; exit 1; fi
  sleep 1
done
[[ $status == 200 ]] && jq -e '.status == "ok" and .db == "ok"' "$dir/body" >/dev/null || {
  printf 'Health did not recover, got %s\n' "$status" >&2; cat "$dir/server.log" >&2; exit 1;
}
printf 'Health degraded and recovered after the database became reachable\n'
