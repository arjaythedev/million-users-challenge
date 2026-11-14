#!/usr/bin/env python3
"""Connection churn next to long-lived connections.

Churn clients send a request and close (FIN or RST) while the response is in flight, over and over, so the server
keeps closing connections that still have a receive or a send outstanding and its freed slots are taken by the next
accept at once. Canary connections stay open the whole time and make requests; none of them may ever see a closed or
short response. (A SHUTDOWN issued for a closed connection that runs after its slot was reused would hit a canary.)

usage: closechurn.py PORT [EXTRA_PORT] [--seconds N] [--canaries N] [--churners N]
"""
import argparse
import socket
import struct
import sys
import threading
import time

ap = argparse.ArgumentParser()
ap.add_argument("port", type=int)
ap.add_argument("extra", type=int, nargs="?")
ap.add_argument("--seconds", type=float, default=20)
ap.add_argument("--canaries", type=int, default=64)
ap.add_argument("--churners", type=int, default=48)
ap.add_argument("--host", default="127.0.0.1")
a = ap.parse_args()
ports = [a.port] + ([a.extra] if a.extra else [])

stop = threading.Event()
lock = threading.Lock()
stats = {"canary_ok": 0, "canary_fail": 0, "churn": 0}
errors = []


def read_response(s, buf=b""):
    while b"\r\n\r\n" not in buf:
        d = s.recv(65536)
        if not d:
            raise EOFError("closed before the head was complete")
        buf += d
    head, _, rest = buf.partition(b"\r\n\r\n")
    status = int(head.split(b" ", 2)[1])
    n = 0
    for line in head.split(b"\r\n")[1:]:
        k, _, v = line.partition(b":")
        if k.strip().lower() == b"content-length":
            n = int(v)
    while len(rest) < n:
        d = s.recv(65536)
        if not d:
            raise EOFError("closed mid-body")
        rest += d
    return status, rest[:n]


def canary(i):
    port = ports[i % len(ports)]
    while not stop.is_set():
        try:
            s = socket.create_connection((a.host, port))
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            s.settimeout(10)
            n = 0
            while not stop.is_set():
                path = b"/feed" if n % 3 == 0 else b"/health"
                s.sendall(b"GET " + path + b" HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\n\r\n")
                st, _ = read_response(s)
                if st != 200:
                    raise RuntimeError("status %d" % st)
                with lock:
                    stats["canary_ok"] += 1
                n += 1
                time.sleep(0.002)
            s.close()
        except Exception as e:  # a canary must never lose its connection: count it and reconnect
            with lock:
                stats["canary_fail"] += 1
                if len(errors) < 5:
                    errors.append("canary %d: %s: %s" % (i, type(e).__name__, e))
            time.sleep(0.05)


def churner(i):
    port = ports[i % len(ports)]
    k = 0
    while not stop.is_set():
        try:
            s = socket.create_connection((a.host, port))
            if k % 2:  # RST instead of FIN
                s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            if k % 5 == 4:
                s.sendall(b"GET /fee")  # an incomplete request, then close
            else:
                s.sendall(b"GET /feed HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\n\r\n" * (1 + k % 3))
            if k % 4 == 3:
                time.sleep(0.0005)
            s.close()
            k += 1
            with lock:
                stats["churn"] += 1
        except OSError:
            time.sleep(0.01)


ts = [threading.Thread(target=canary, args=(i,), daemon=True) for i in range(a.canaries)]
ts += [threading.Thread(target=churner, args=(i,), daemon=True) for i in range(a.churners)]
for t in ts:
    t.start()
time.sleep(a.seconds)
stop.set()
time.sleep(0.5)
print("canary requests ok=%d failed=%d, churned connections=%d" % (stats["canary_ok"], stats["canary_fail"], stats["churn"]))
for e in errors:
    print("  " + e)
sys.exit(1 if stats["canary_fail"] else 0)
