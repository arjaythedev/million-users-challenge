#!/usr/bin/env bash
# Provision the Lima VM (Ubuntu 24.04, 12 vCPU, 18 GiB) as a one-machine stand-in for app box + db box:
#   cpus 0-3  the server (own systemd scope)        -> the 4-core score configuration
#   cpus 4-7  Postgres 18 (systemd AllowedCPUs)     -> box/postgresql.conf, memory scaled to the VM
#   cpus 8-11 mload (taskset in capacity.sh)
# Kernel settings are box/setup-app-box.sh's sysctls (the NIC/RPS/qdisc parts don't apply to loopback).
#
# On the Mac (results land in profiles/linux, which is gitignored; create it first):
#   mkdir -p <repo>/submissions/go-hertz-molarom/profiles/linux
#   limactl create --name=mucbox --cpus=12 --memory=18 --disk=60 --vm-type=vz \
#     --set '.mounts=[{"location":"<repo>","writable":false},{"location":"<repo>/submissions/go-hertz-molarom/profiles/linux","mountPoint":"/results","writable":true}]' \
#     template:ubuntu-24.04 && limactl start mucbox
#   limactl shell mucbox sudo bash <repo>/submissions/go-hertz-molarom/bench/linux-vm.sh
# Then: limactl shell mucbox sudo bash /opt/muc/submissions/go-hertz-molarom/bench/capacity.sh ...
#
# The server is built and run from the copy in /opt/muc, not the mount. `linux-vm.sh sync` re-copies the
# mount and rebuilds without re-provisioning; capacity.sh runs it on every start, so edits on the Mac
# are always what gets benchmarked.
set -euo pipefail
SRC="$(cd "$(dirname "$0")/../../.." && pwd)"   # the read-only repo mount
DST=/opt/muc
GO_VERSION=${GO_VERSION:-1.27.3}
ARCH=$(dpkg --print-architecture)

# Mirror the mount into $DST. The VM's own seed (tokens.json and tsv, generated here for feed_seed) and
# build outputs are excluded, so they are neither overwritten by the Mac's copies nor deleted.
sync_repo() {
  echo "== repo copy $SRC -> $DST"
  mkdir -p "$DST"
  rsync -a --delete --exclude '.git' --exclude 'bin/' --exclude 'bench/mload/mload' \
    --exclude 'seed/tokens.json' --exclude 'seed/tsv/' \
    --exclude 'submissions/go-hertz-molarom/profiles/cap[0-9]*' --exclude 'submissions/go-hertz-molarom/profiles/linux' \
    --exclude 'node_modules' "$SRC/" "$DST/"
  echo "$SRC" > "$DST/.muc-src"   # lets capacity.sh find the mount to re-sync from
}

build() {
  echo "== build"
  (cd "$DST/bench/mload" && go build -o mload .)
  bash "$DST/submissions/go-hertz-molarom/build.sh"
  mkdir -p "$DST/submissions/go-hertz-molarom/profiles"
  ln -sfn /results "$DST/submissions/go-hertz-molarom/profiles/linux"
}

if [ "${1:-}" = sync ]; then
  sync_repo
  build
  exit 0
fi

echo "== packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq postgresql-common curl ca-certificates jq python3 rsync nodejs >/dev/null   # nodejs: seed/make-seed.sh
if ! dpkg -s postgresql-18 >/dev/null 2>&1; then
  /usr/share/postgresql-common/pgdg/apt.postgresql.org.sh -y >/dev/null
  apt-get install -y -qq postgresql-18 >/dev/null
fi

echo "== go $GO_VERSION ($ARCH)"
if [ "$(/usr/local/go/bin/go env GOVERSION 2>/dev/null)" != "go$GO_VERSION" ]; then
  curl -fsSL "https://go.dev/dl/go$GO_VERSION.linux-$ARCH.tar.gz" -o /tmp/go.tgz
  rm -rf /usr/local/go && tar -C /usr/local -xzf /tmp/go.tgz && rm -f /tmp/go.tgz
fi
ln -sf /usr/local/go/bin/go /usr/local/bin/go

echo "== kernel limits (box/setup-app-box.sh sysctls)"
cat > /etc/sysctl.d/90-million.conf <<'S'
fs.nr_open = 16777216
fs.file-max = 33554432
net.ipv4.ip_local_port_range = 1024 65535
net.core.somaxconn = 65535
net.ipv4.tcp_max_syn_backlog = 65535
net.core.netdev_max_backlog = 250000
net.ipv4.tcp_tw_reuse = 1
net.ipv4.tcp_mem = 1572864 2097152 3145728
net.ipv4.tcp_rmem = 4096 16384 4194304
net.ipv4.tcp_wmem = 4096 16384 4194304
net.ipv4.tcp_max_orphans = 1048576
net.core.rps_sock_flow_entries = 65536
S
sysctl -q --system
cat > /etc/security/limits.d/90-million.conf <<'L'
* soft nofile 8388608
* hard nofile 8388608
root soft nofile 8388608
root hard nofile 8388608
L

sync_repo

echo "== postgres 18 (box/postgresql.conf, memory scaled to an 18 GiB VM shared with server + mload)"
CONF=/etc/postgresql/18/main
install -m 644 "$DST/box/postgresql.conf" $CONF/conf.d/challenge.conf
cat > $CONF/conf.d/zz-vm.conf <<'P'
# VM overrides: the real db box has 32 GB to itself
shared_buffers = 2GB
effective_cache_size = 4GB
maintenance_work_mem = 256MB
max_wal_size = 4GB
min_wal_size = 1GB
huge_pages = off
listen_addresses = '127.0.0.1'
P
grep -q 'feed,feed_seed feed 127.0.0.1/32' $CONF/pg_hba.conf || \
  echo "host feed,feed_seed feed 127.0.0.1/32 scram-sha-256" >> $CONF/pg_hba.conf
mkdir -p /etc/systemd/system/postgresql@18-main.service.d
cat > /etc/systemd/system/postgresql@18-main.service.d/cpus.conf <<'U'
[Service]
AllowedCPUs=4-7
U
systemctl daemon-reload
systemctl restart postgresql@18-main
sudo -u postgres psql -qX -c "DO \$\$BEGIN CREATE ROLE feed LOGIN CREATEDB PASSWORD 'feed'; EXCEPTION WHEN duplicate_object THEN NULL; END\$\$"

echo "== seed (feed_seed + tokens.json)"
if ! sudo -u postgres psql -qXAt -d postgres -c "SELECT 1 FROM pg_database WHERE datname='feed_seed'" | grep -q 1; then
  rm -rf /tmp/seed-build && cp -r "$DST/seed" /tmp/seed-build && cp "$DST/schema.sql" /tmp/ && chown -R postgres: /tmp/seed-build /tmp/schema.sql
  sudo -u postgres PGUSER=postgres bash /tmp/seed-build/make-seed.sh
  sudo -u postgres psql -qX -c "ALTER DATABASE feed_seed OWNER TO feed" -c "ALTER DATABASE feed_seed WITH ALLOW_CONNECTIONS false"
  cp /tmp/seed-build/tokens.json "$DST/seed/tokens.json"
fi
[ -f "$DST/seed/tokens.json" ] || cp /tmp/seed-build/tokens.json "$DST/seed/tokens.json"
# The seed tables are created by postgres; the server connects as feed and needs to read and write them.
sudo -u postgres psql -qX -c "ALTER DATABASE feed_seed WITH ALLOW_CONNECTIONS true"
sudo -u postgres psql -qX -d feed_seed -c "GRANT ALL ON ALL TABLES IN SCHEMA public TO feed" \
  -c "GRANT ALL ON ALL SEQUENCES IN SCHEMA public TO feed" -c "GRANT ALL ON SCHEMA public TO feed"
sudo -u postgres psql -qX -c "ALTER DATABASE feed_seed WITH ALLOW_CONNECTIONS false"

build
echo "ready: $(nproc) cpus, $(free -g | awk '/^Mem:/{print $2}') GiB, postgres cpus $(cat /sys/fs/cgroup/system.slice/system-postgresql.slice/postgresql@18-main.service/cpuset.cpus 2>/dev/null)"
