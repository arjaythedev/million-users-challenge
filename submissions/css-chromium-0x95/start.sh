#!/usr/bin/env bash
# Runs the server in the foreground (systemd ExecStart). Config (SPEC.md runtime contract): DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT.
# Optional tuning (see README.md "Configuration"): CSS_HEAVY / CSS_LIGHT (Chromium pages for requests with / without an Authorization
# header; default nproc-2 / 1 or 2), CSS_WORKERS (total, overrides the heavy count), CSS_HEAVY_BATCH, CSS_QUEUE_HEAVY, CSS_HEAVY_NICE,
# CSS_GC_EVERY, CSS_STATS=1, CSS_WS_PORT (9900), CHROME_BIN, PG_CONNS (32, spread over the pages; the pages speak Postgres, the front only relays bytes), CSS_HOLD_MS (8), FRONT_THREADS (2).
set -euo pipefail
# soft open-files limit = hard limit: a no-op under systemd (LimitNOFILE sets both), needed from a login shell
ulimit -n "$(ulimit -Hn)" 2>/dev/null || true
exec "$(cd "$(dirname "$0")" && pwd)/target/release/css-front"
