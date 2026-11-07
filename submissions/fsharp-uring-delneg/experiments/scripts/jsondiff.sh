#!/usr/bin/env bash
# jsondiff.sh <dirA> <dirB> [ENV=VAL ...]: (the env goes to B) same fresh database (with odd posts added), each build answers /feed, /feed gzipped and
# /posts/1..300 plus the odd posts; the answers must be byte-identical
set -u
A=$1; B=$2; shift 2; ENVB=("$@")
sudo -u postgres psql -qX -c "DROP DATABASE IF EXISTS feed_man WITH (FORCE)" -c "CREATE DATABASE feed_man OWNER feed TEMPLATE feed_seed" >/dev/null
sudo -u postgres psql -qX -d feed_man >/dev/null <<'SQL'
INSERT INTO posts (user_id, body, created_at) VALUES
 (1, E'quote " backslash \\ tab \t nl \n cr \r ctl \x01 \x1f del \x7f', '2026-10-28 23:59:59.999999+00'),
 (2, E'emoji 😀 ü 中文 ‮rtl', '1969-12-31 23:59:59.123456+00'),
 (3, E'x', '2101-03-01 00:00:00.000999+00'),
 (4, repeat('long ', 100), '2000-02-29 12:34:56.789+00'),
 (5, E'</script> & < >    ', now());
SQL
IDS=$(sudo -u postgres psql -qXtA -d feed_man -c "SELECT string_agg(id::text, ' ') FROM (SELECT id FROM posts ORDER BY id DESC LIMIT 5) t")
for side in A B; do
  dir=${!side}; extra=(); [ $side = B ] && extra=("${ENVB[@]}")
  sudo -u challenge env "${extra[@]}" DATABASE_URL=postgres://feed:feed@127.0.0.1:5432/feed_man JWT_SECRET=million-users-challenge HOST=127.0.0.1 PORT=3100 bash -c "cd $dir && exec bash start.sh" > /tmp/jd-$side.log 2>&1 &
  for i in $(seq 60); do curl -sf http://127.0.0.1:3100/health >/dev/null && break; sleep 0.5; done
  out=/tmp/jd-$side; rm -rf $out; mkdir -p $out
  curl -s http://127.0.0.1:3100/feed > $out/feed
  curl -s -H 'Accept-Encoding: gzip' http://127.0.0.1:3100/feed | gunzip -c > $out/feed.gz.txt
  for id in $(seq 1 300) $IDS 0 999999999; do curl -s -i http://127.0.0.1:3100/posts/$id | tr -d '\r' >> $out/posts; done
  pkill -f "$dir/out/Server"; sleep 1
done
sudo -u postgres psql -qX -c "DROP DATABASE IF EXISTS feed_man WITH (FORCE)" >/dev/null
for f in feed feed.gz.txt posts; do cmp -s /tmp/jd-A/$f /tmp/jd-B/$f && echo "$f same ($(stat -c %s /tmp/jd-A/$f) B)" || echo "$f DIFFERENT"; done
head -c 700 /tmp/jd-B/feed; echo
