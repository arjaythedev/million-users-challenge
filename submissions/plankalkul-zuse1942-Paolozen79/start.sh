#!/usr/bin/env bash
# Runs the Plankalkul server in the foreground.
# Configured by DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT.
exec "$(dirname "$0")/bin/server"
