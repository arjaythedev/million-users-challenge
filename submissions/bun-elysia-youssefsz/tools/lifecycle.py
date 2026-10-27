#!/usr/bin/env python3
"""Verify real TCP keep-alive, draining an in-flight write, and worker failure propagation."""

import base64
import hashlib
import hmac
import http.client
import json
import pathlib
import socket
import subprocess
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[3]
NAME = "million-lifecycle"


def command(*args):
    return subprocess.check_output(args, text=True).strip()


def start():
    subprocess.run(
        ["docker", "rm", "-f", NAME],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    command(
        "docker",
        "run",
        "-d",
        "--name",
        NAME,
        "--network",
        "million-bench",
        "--cpus",
        "2",
        "--memory",
        "512m",
        "-v",
        f"{ROOT}:/challenge",
        "-w",
        "/challenge/submissions/bun-elysia-youssefsz",
        "-e",
        "DATABASE_URL=postgres://postgres:challenge-local@million-postgres:5432/feed_dev",
        "-e",
        "JWT_SECRET=million-users-challenge",
        "-e",
        "HOST=0.0.0.0",
        "-e",
        "PORT=3000",
        "-e",
        "EXTRA_PORT=3001",
        "-p",
        "127.0.0.1:18084:3000",
        "-p",
        "127.0.0.1:18085:3001",
        "million-bun:1.4.2",
        "bun",
        "dist/main.js",
        "--workers",
        "2",
        "--pool",
        "16",
    )
    for port in (18084, 18085):
        for _ in range(100):
            try:
                connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
                connection.request("GET", "/health")
                response = connection.getresponse()
                response.read()
                connection.close()
                if response.status == 200:
                    break
            except OSError:
                time.sleep(0.1)
        else:
            raise RuntimeError("No healthy listener")


def main():
    start()
    connection = http.client.HTTPConnection("127.0.0.1", 18084, timeout=5)
    connection.request("GET", "/health")
    assert connection.getresponse().read()
    original = connection.sock
    time.sleep(80)
    assert original is not None
    original.setblocking(False)
    try:
        assert original.recv(1, socket.MSG_PEEK) != b"", "Server closed the idle socket"
    except BlockingIOError:
        pass
    finally:
        original.settimeout(5)
    connection.request("GET", "/health")
    response = connection.getresponse()
    assert response.status == 200
    response.read()
    assert connection.sock is original, "Client reconnected"
    connection.close()
    print("PASS same TCP connection survives 80 seconds idle", flush=True)

    lock = subprocess.Popen(
        [
            "docker",
            "exec",
            "-i",
            "million-postgres",
            "psql",
            "-qAt",
            "-U",
            "postgres",
            "-d",
            "feed_dev",
            "-v",
            "ON_ERROR_STOP=1",
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    assert lock.stdin is not None and lock.stdout is not None
    lock.stdin.write("BEGIN; LOCK TABLE posts IN SHARE MODE; SELECT 'locked';\n")
    lock.stdin.flush()
    assert lock.stdout.readline().strip() == "locked"
    encode = lambda value: base64.urlsafe_b64encode(value).rstrip(b"=").decode()
    header = encode(b'{"alg":"HS256"}')
    payload = encode(
        json.dumps(
            {"sub": "1", "username": "drain-test", "exp": int(time.time()) + 60}
        ).encode()
    )
    signature = encode(
        hmac.new(
            b"million-users-challenge", f"{header}.{payload}".encode(), hashlib.sha256
        ).digest()
    )
    result = {}

    def write_post():
        try:
            client = http.client.HTTPConnection("127.0.0.1", 18085, timeout=10)
            client.request(
                "POST",
                "/posts",
                body='{"body":"shutdown durability test"}',
                headers={
                    "Authorization": f"Bearer {header}.{payload}.{signature}",
                    "Content-Type": "application/json",
                },
            )
            response = client.getresponse()
            result.update(status=response.status, body=json.loads(response.read()))
            client.close()
        except (OSError, http.client.HTTPException, ValueError) as error:
            result["error"] = str(error)

    thread = threading.Thread(target=write_post)
    thread.start()
    try:
        for _ in range(50):
            blocked = command(
                "docker",
                "exec",
                "million-postgres",
                "psql",
                "-qAt",
                "-U",
                "postgres",
                "-d",
                "feed_dev",
                "-c",
                "SELECT count(*) FROM pg_stat_activity WHERE datname='feed_dev' AND application_name='bun-elysia-feed' AND wait_event_type='Lock'",
            )
            if int(blocked) > 0:
                break
            time.sleep(0.02)
        else:
            raise RuntimeError("Write did not reach the database lock")
        command("docker", "kill", "--signal", "TERM", NAME)
        lock.stdin.write("COMMIT;\n\\q\n")
        lock.stdin.flush()
        lock.wait(timeout=5)
        thread.join(timeout=10)
        assert result.get("status") == 201, result
        post_id = result["body"]["post"]["id"]
        persisted = command(
            "docker",
            "exec",
            "million-postgres",
            "psql",
            "-qAt",
            "-U",
            "postgres",
            "-d",
            "feed_dev",
            "-c",
            f"SELECT count(*) FROM posts WHERE id={int(post_id)} AND body='shutdown durability test'",
        )
        assert persisted == "1"
        assert command("docker", "wait", NAME) == "0"
        print(
            "PASS SIGTERM drains the in-flight write and preserves the committed row",
            flush=True,
        )
    finally:
        if lock.poll() is None:
            lock.stdin.close()
            lock.wait(timeout=5)

    start()
    command(
        "docker",
        "exec",
        NAME,
        "sh",
        "-c",
        'set -- $(cat /proc/1/task/1/children); kill -KILL "$1"',
    )
    assert command("docker", "wait", NAME) == "1"
    print("PASS worker crash stops the whole service with a failure exit", flush=True)
    command("docker", "rm", NAME)


if __name__ == "__main__":
    main()
