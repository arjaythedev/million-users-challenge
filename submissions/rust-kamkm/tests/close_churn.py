#!/usr/bin/env python3
"""Check that closing many idle connections does not stall health requests.

Run against a dedicated server and load-generator host with sufficient file descriptors and
destination addresses or forwarded ports. This is a load test, not a quick unit test.
"""

import argparse
import socket
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("--ports", required=True, help="comma-separated destination ports")
    parser.add_argument("--connections", type=int, default=200_000)
    parser.add_argument("--max-health-ms", type=float, default=2500)
    args = parser.parse_args()
    ports = [int(port) for port in args.ports.split(",")]
    sockets = []
    try:
        for index in range(args.connections):
            sockets.append(socket.create_connection((args.host, ports[index % len(ports)]), timeout=5))
        print(f"opened {len(sockets)} connections", flush=True)
        # Allow the server to arm the receives before the simultaneous close.
        time.sleep(2)

        def close_group(group):
            for sock in group:
                sock.close()

        workers = [threading.Thread(target=close_group, args=(sockets[index::4],)) for index in range(4)]
        started = time.monotonic()
        for worker in workers:
            worker.start()
        latencies = []
        failures = []
        while any(worker.is_alive() for worker in workers) or time.monotonic() - started < 5:
            before = time.monotonic()
            try:
                with socket.create_connection((args.host, ports[0]), timeout=3) as sock:
                    sock.settimeout(3)
                    sock.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
                    response = sock.recv(1024)
                    if not response.startswith(b"HTTP/1.1 200"):
                        failures.append(repr(response[:100]))
            except OSError as error:
                failures.append(str(error))
            latencies.append((time.monotonic() - before) * 1000)
        for worker in workers:
            worker.join()
        worst = max(latencies)
        print(f"max health latency {worst:.1f} ms; failures {len(failures)}", flush=True)
        if failures or worst > args.max_health_ms:
            raise SystemExit(f"FAIL: health stalled during connection churn: {failures[:2]}")
        print("PASS: health remained responsive during connection churn")
    finally:
        for sock in sockets:
            sock.close()


if __name__ == "__main__":
    main()
