#!/usr/bin/env bash
# The challenge's test/run.sh (42 checks) on the box, on PORT (default 3000), then again with PORT/EXTRA_PORT swapped,
# from a clean copy of /opt/muc-aspnet/current in /opt/muc-aspnet/repo/submissions/ (fresh feed_test each time).
set -u
R=/opt/muc-aspnet/repo; S=$R/submissions/fsharp-aspnetcore-delneg
rm -rf "$S"; mkdir -p "$S"
rsync -a --exclude bin --exclude obj --exclude out /opt/muc-aspnet/current/ "$S/"
chown -R challenge: "$R"
cd "$R"
for ports in "3000 3001" "3001 3000"; do
  set -- $ports
  sudo -u challenge -H env PGHOST=127.0.0.1 PGUSER=feed PGPASSWORD=feed PORT=$1 EXTRA_PORT=$2 \
    bash test/run.sh submissions/fsharp-aspnetcore-delneg 2>&1 | grep -E "FAIL|want|got|passed=|error" 
done
