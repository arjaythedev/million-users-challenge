#!/usr/bin/env python3
"""Linux regression for continuation deduplication and descriptor backpressure.

Uses Python's standard library and a built reactor binary. DATABASE_URL must name
an available challenge database. Only /health and an unknown route are requested;
no data is written. Two temporary child servers run sequentially on alternate
ports. The second child alone gets a reduced RLIMIT_NOFILE; the caller's limits
and the machine's limits are unchanged. Every child/socket is cleaned up.

  DATABASE_URL=postgres://... JWT_SECRET=... python3 tests/worker-check.py \
    --binary ./out/FeedApi --backend epoll --port 3300 --extra-port 3301
  # Repeat with --backend io-uring to exercise the optional backend.

The continuation assertion uses the worker's shutdown high-water counter against
one real, continuously pipelined connection. The descriptor case proves actual
EMFILE/ENFILE occurred, checks an existing database-backed connection during that
condition, bounds retry/log counts, then verifies new accepts recover.
"""

import argparse
import json
import math
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time


REQUEST = b"GET /worker-regression-missing HTTP/1.1\r\nHost: test\r\n\r\n"
HEALTH = b"GET /health HTTP/1.1\r\nHost: test\r\n\r\n"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


class Client:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=3)
        self.input = self.socket.makefile("rb")

    def response(self):
        line = self.input.readline(4096)
        require(line.startswith(b"HTTP/1.1 "), f"Invalid or missing status line: {line!r}")
        status = int(line.split(b" ", 2)[1])
        headers = {}
        for _ in range(32):
            line = self.input.readline(4096)
            if line == b"\r\n":
                break
            require(line.endswith(b"\r\n") and b":" in line, "Invalid response header")
            key, value = line.split(b":", 1)
            headers[key.lower()] = value.strip()
        else:
            raise AssertionError("Too many response headers")
        length = int(headers[b"content-length"])
        require(0 <= length <= 16384, "Unexpected response size")
        body = self.input.read(length)
        require(len(body) == length, "Truncated response body")
        return status, json.loads(body)

    def health(self):
        started = time.monotonic()
        self.socket.sendall(HEALTH)
        status, body = self.response()
        require(status == 200 and body.get("status") == "ok" and body.get("db") == "ok",
                f"Health failed: {status} {body}")
        return time.monotonic() - started

    def close(self):
        self.input.close()
        self.socket.close()


class Child:
    def __init__(self, args, nofile=None):
        self.args = args
        self.nofile = nofile
        self.process = None
        self.temporary = None
        self.output = None
        self.log = ""
        self.started = 0
        self.stopped = 0

    def __enter__(self):
        import resource  # Linux-only; keep syntax checks usable on other platforms.

        self.temporary = tempfile.TemporaryDirectory(prefix="reactor-worker-check-")
        self.path = Path(self.temporary.name) / "server.log"
        self.output = self.path.open("wb", buffering=0)
        env = dict(os.environ, HOST="127.0.0.1", PORT=str(self.args.port),
                   EXTRA_PORT=str(self.args.extra_port), DOTNET_PROCESSOR_COUNT="1")
        env.setdefault("JWT_SECRET", "worker-regression-only")
        command = [str(self.args.binary)]
        if self.args.backend == "io-uring":
            command.append("--io-uring")

        def child_limit():
            if self.nofile is not None:
                resource.setrlimit(resource.RLIMIT_NOFILE, (self.nofile, self.nofile))

        try:
            self.started = time.monotonic()
            self.process = subprocess.Popen(command, cwd=self.args.binary.parent, env=env,
                                            stdin=subprocess.DEVNULL, stdout=self.output,
                                            stderr=subprocess.STDOUT, preexec_fn=child_limit)
            return self
        except BaseException:
            self.output.close()
            self.temporary.cleanup()
            raise

    def connect(self):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            require(self.process.poll() is None, "Child server exited before accepting connections")
            try:
                client = Client(self.args.port)
            except ConnectionRefusedError:
                time.sleep(0.05)
                continue
            try:
                client.health()
                return client
            except BaseException:
                client.close()
                raise
        raise AssertionError("Child server did not become ready within 20 seconds")

    def current_log(self):
        return self.path.read_text(errors="replace")

    def __exit__(self, error_type, error, traceback):
        try:
            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=5)
            self.stopped = time.monotonic()
            self.output.close()
            self.log = self.current_log()
            if error_type is not None or self.process.returncode != 0:
                print(self.log[-16384:], file=sys.stderr)
            if error_type is None:
                require(self.process.returncode == 0,
                        f"Child server exited with {self.process.returncode}")
        finally:
            self.temporary.cleanup()

    def stats(self):
        lines = re.findall(r"^worker=0 responses=.*$", self.log, re.MULTILINE)
        require(len(lines) == 1, "Missing or duplicate worker shutdown counters")
        return {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", lines[0])}


def check_continuations(args):
    with Child(args) as child:
        client = child.connect()
        try:
            # Each burst exceeds the initial receive buffer but stays well below
            # the request-buffer cap, forcing repeated Read/Dispatch/Flush paths.
            for _ in range(16):
                client.socket.sendall(REQUEST * 256)
                for _ in range(256):
                    status, body = client.response()
                    require(status == 404 and body == {"error": "not found"},
                            f"Pipelined response was lost or corrupted: {status} {body}")
            client.health()
        finally:
            client.close()
    stats = child.stats()
    require(stats["accepted_clients"] == 1, "Continuation test did not use exactly one client")
    require(stats["responses"] == 4098, "Unexpected pipelined response count")
    require(stats["max_continuations"] == 1,
            f"One connection created duplicate continuation entries: {stats}")
    require(stats["listener_backoffs"] == 0, "Unexpected descriptor exhaustion in pipeline test")
    return stats


def check_descriptors(args):
    sockets = []
    latencies = []
    with Child(args, args.nofile) as child:
        client = child.connect()
        pressure_started = time.monotonic()
        held = 0
        try:
            # Idle TCP clients avoid database overload. Connections beyond the
            # child's descriptor budget remain in the listener backlog.
            for index in range(args.nofile * 2):
                port = args.port if index % 2 == 0 else args.extra_port
                sockets.append(socket.create_connection(("127.0.0.1", port), timeout=2))
            held = len(sockets)
            deadline = time.monotonic() + 4
            while time.monotonic() < deadline:
                require(child.process.poll() is None, "Server exited under descriptor pressure")
                if re.search(r"accept worker=0 errno=(23|24)\b", child.current_log()):
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("Reduced descriptor limit did not produce EMFILE/ENFILE")
            for _ in range(10):
                latencies.append(client.health())
                time.sleep(0.1)
            for connection in sockets:
                connection.close()
            sockets.clear()
            # Opening a fresh client must recover automatically after descriptors
            # become available, without restarting or disturbing the old client.
            recovered = Client(args.port)
            try:
                recovered.health()
            finally:
                recovered.close()
            latencies.append(client.health())
        finally:
            for connection in sockets:
                connection.close()
            client.close()
    stats = child.stats()
    require(stats["listener_backoffs"] > 0, "Descriptor backpressure was not recorded")
    require(stats["listener_backoffs"] <= math.ceil((child.stopped - pressure_started) * 10) + 2,
            f"Listener retries exceeded the 100 ms backoff budget: {stats}")
    expected_logs = 1 + math.ceil((child.stopped - child.started) / 5)
    require(stats["accept_error_logs"] <= expected_logs, f"Repeated accept errors were not bounded: {stats}")
    actual_logs = re.findall(r"^accept worker=0 errno=\d+$", child.log, re.MULTILINE)
    require(len(actual_logs) == stats["accept_error_logs"], "Accept error log counter differs from actual output")
    return dict(stats, held_connections=held, max_existing_health_latency_s=max(latencies))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--backend", choices=["epoll", "io-uring"], default="epoll")
    parser.add_argument("--port", type=int, default=3300)
    parser.add_argument("--extra-port", type=int, default=3301)
    parser.add_argument("--nofile", type=int, default=128)
    args = parser.parse_args()
    require(sys.platform == "linux", "Native worker regressions require Linux")
    require(os.environ.get("DATABASE_URL"), "DATABASE_URL must name a ready challenge test database")
    require(96 <= args.nofile <= 256, "--nofile must be between 96 and 256")
    require(args.port != args.extra_port and all(1024 <= port <= 65535 for port in (args.port, args.extra_port)),
            "Use two distinct unprivileged alternate ports")
    args.binary = args.binary.resolve(strict=True)
    # Refuse occupied ports so these tests cannot accidentally target another app.
    for port in (args.port, args.extra_port):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", port))
    result = {"test": "reactor-worker-lifecycle", "backend": args.backend,
              "continuations": check_continuations(args), "descriptors": check_descriptors(args)}
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"worker regression failed: {error}", file=sys.stderr)
        sys.exit(1)
