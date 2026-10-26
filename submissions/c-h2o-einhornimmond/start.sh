#!/usr/bin/env bash
# Runs the server in the foreground. Config: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT.
# DB_POOL_SIZE is the number of PostgreSQL connections, the most rule 5 allows by default.
export DB_POOL_SIZE="${DB_POOL_SIZE:-512}"
exec "$(dirname "$0")/bin/server"
