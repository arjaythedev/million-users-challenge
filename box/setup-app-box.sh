#!/usr/bin/env bash
# The app box (16 vCPU, 32 GB, Ubuntu 24.04): kernel limits for ~1M concurrent TCP connections, and the user and
# directories your server runs with. This is everything that is tuned on the box (rule 12). Run once as root.
set -euo pipefail
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
# The VM's NIC has one queue: spread receive processing over every core (RPS).
NIC=$(ip -o route get 1.1.1.1 | grep -o 'dev [^ ]*' | cut -d' ' -f2)
printf '%x' $(( (1 << $(nproc)) - 1 )) > /sys/class/net/$NIC/queues/rx-0/rps_cpus
echo 65536 > /sys/class/net/$NIC/queues/rx-0/rps_flow_cnt
# fq instead of fq_codel: with hundreds of thousands of flows behind one small TX ring, fq_codel drops packets
# (each drop is a TCP retransmit); fq queues the bursts.
tc qdisc replace dev $NIC root fq limit 200000 flow_limit 1000 buckets 262144
id challenge >/dev/null 2>&1 || useradd -m -s /bin/bash challenge
mkdir -p /opt/challenge /etc/challenge && chown challenge: /opt/challenge
install -m 644 "$(dirname "$0")/challenge.service" /etc/systemd/system/challenge.service
systemctl daemon-reload
echo "app box ready: nr_open=$(sysctl -n fs.nr_open), rps on $NIC, qdisc fq"
