#!/usr/bin/env bash
# In the app container (needs tcpdump). ackcount.sh SECONDS: the packets the server sends its clients (ports 3000
# and 3001) over the window, and how many of them are pure ACKs: no payload, no SYN, FIN or RST.
t=${1:-30}
timeout "$t" tcpdump -i any -nn -s 96 -w /tmp/ack.pcap 'tcp and (src port 3000 or src port 3001)' 2>/dev/null
total=$(tcpdump -nn -r /tmp/ack.pcap 2>/dev/null | wc -l)
pure=$(tcpdump -nn -r /tmp/ack.pcap \
  '(tcp[tcpflags] & (tcp-syn|tcp-fin|tcp-rst)) == 0 and (ip[2:2] - ((ip[0] & 0xf) << 2) - ((tcp[12] & 0xf0) >> 2)) == 0' \
  2>/dev/null | wc -l)
syn=$(tcpdump -nn -r /tmp/ack.pcap '(tcp[tcpflags] & tcp-syn) != 0' 2>/dev/null | wc -l)
echo "to clients: $total packets, $pure pure ACKs, $syn SYN-ACKs"
