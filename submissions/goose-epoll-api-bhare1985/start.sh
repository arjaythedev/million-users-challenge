#!/usr/bin/env bash
# Runs the server in the foreground. Config: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT (SPEC.md). The API it serves is
# described by openapi.json beside this script; API_CONFIG may name another OpenAPI file, and is not needed for the benchmark.
here="$(dirname "$0")"
export API_CONFIG="${API_CONFIG:-$here/openapi.json}"
exec "$here/bin/server"
