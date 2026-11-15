#!/usr/bin/env bash
# Runs the server in the foreground. Reads DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT from the environment.
DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$DIR/build/server"
