#!/usr/bin/env bash
DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$DIR/target/release/pulse"
