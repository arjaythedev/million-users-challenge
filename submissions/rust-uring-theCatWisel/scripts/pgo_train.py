#!/usr/bin/env python3
import base64
import hashlib
import hmac
import http.client
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import threading
import time


def pg_frame(tag: bytes, body: bytes) -> bytes:
    return tag + struct.pack("!I", 4 + len(body)) + body


def pg_col(data: bytes) -> bytes:
    return struct.pack("!i", len(data)) + data


def build_feed_rows() -> bytes:
    pg_us = (1_770_000_000 - 946_684_800) * 1_000_000 + 123_000
    samples = [
        "Latency numbers from the morning benchmark looked clean until queue depth crossed 4096 sockets.",
        "Switching from text result columns to binary int8 and timestamptz cut parsing overhead noticeably.",
        "Coffee at 06:30 before checking p99 curves across four cores and fifty-one gigabytes of dataset.",
        "Batching write flushes behind ReadyForQuery preserves durability while keeping WAL lock contention low.",
        "Vectorized JSON escaping handles embedded \"quotes\" and control bytes without branch mispredictions.",
    ]
    chunks = [pg_frame(b"2", b"")]
    for idx in range(20):
        pid = struct.pack("!q", 500_000 - idx)
        ts_bin = struct.pack("!q", pg_us - idx * 1_350_000)
        s_a = samples[idx % len(samples)]
        s_b = samples[(idx + 2) % len(samples)]
        body = f"{s_a} {s_b} (entry {idx})".encode()
        author = f"user_{(12_340 + idx * 137) % 100_000}".encode()
        likes = struct.pack("!i", (idx * 19 + 3) % 97)
        row = (
            struct.pack("!H", 5)
            + pg_col(pid)
            + pg_col(body)
            + pg_col(ts_bin)
            + pg_col(author)
            + pg_col(likes)
        )
        chunks.append(pg_frame(b"D", row))
    chunks.append(pg_frame(b"C", b"SELECT 20\x00"))
    return b"".join(chunks)


FEED_REPLY = build_feed_rows()
SINGLE_POST_REPLY = (
    pg_frame(b"2", b"")
    + pg_frame(
        b"D",
        struct.pack("!H", 5)
        + pg_col(struct.pack("!q", 42))
        + pg_col(b"Detailed post body with escaped \"segments\" and newline\ntext.")
        + pg_col(struct.pack("!q", 823_315_200_456_000))
        + pg_col(b"user_042")
        + pg_col(struct.pack("!i", 12)),
    )
    + pg_frame(b"C", b"SELECT 1\x00")
)
NOT_FOUND_POST_REPLY = pg_frame(b"2", b"") + pg_frame(b"C", b"SELECT 0\x00")
HEALTH_REPLY = (
    pg_frame(b"2", b"")
    + pg_frame(b"D", struct.pack("!H", 1) + pg_col(b"1"))
    + pg_frame(b"C", b"SELECT 1\x00")
)
CREATE_POST_REPLY = (
    pg_frame(b"2", b"")
    + pg_frame(
        b"D",
        struct.pack("!H", 2)
        + pg_col(struct.pack("!q", 500_001))
        + pg_col(struct.pack("!q", 823_315_200_789_000)),
    )
    + pg_frame(b"C", b"INSERT 0 1\x00")
)
LIKE_REPLY = pg_frame(b"2", b"") + pg_frame(b"C", b"INSERT 0 1\x00")
READY_IDLE = pg_frame(b"Z", b"I")


def handle_pg_conn(conn: socket.socket, stop_ev: threading.Event) -> None:
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    conn.settimeout(0.5)
    try:
        hdr = b""
        while len(hdr) < 4 and not stop_ev.is_set():
            try:
                part = conn.recv(4 - len(hdr))
            except TimeoutError:
                continue
            if not part:
                return
            hdr += part
        slen = struct.unpack("!I", hdr)[0]
        rem = slen - 4
        while rem > 0:
            part = conn.recv(rem)
            if not part:
                return
            rem -= len(part)
        conn.sendall(pg_frame(b"R", struct.pack("!I", 0)) + READY_IDLE)

        rx = bytearray()
        pending_stmt = b""
        out = bytearray()
        while not stop_ev.is_set():
            try:
                chunk = conn.recv(65536)
            except TimeoutError:
                continue
            if not chunk:
                break
            rx.extend(chunk)
            pos = 0
            out.clear()
            while pos + 5 <= len(rx):
                tag = rx[pos : pos + 1]
                mlen = struct.unpack("!I", rx[pos + 1 : pos + 5])[0]
                if pos + 1 + mlen > len(rx):
                    break
                payload = bytes(rx[pos + 5 : pos + 1 + mlen])
                pos += 1 + mlen
                if tag == b"P":
                    out.extend(pg_frame(b"1", b""))
                elif tag == b"B":
                    nul1 = payload.find(b"\x00")
                    nul2 = payload.find(b"\x00", nul1 + 1)
                    pending_stmt = payload[nul1 + 1 : nul2]
                    if pending_stmt == b"g" and len(payload) >= 21:
                        pid = struct.unpack("!q", payload[13:21])[0]
                        if pid == 999999:
                            pending_stmt = b"g404"
                elif tag == b"E":
                    if pending_stmt == b"":
                        out.extend(FEED_REPLY)
                    elif pending_stmt == b"g":
                        out.extend(SINGLE_POST_REPLY)
                    elif pending_stmt == b"g404":
                        out.extend(NOT_FOUND_POST_REPLY)
                    elif pending_stmt == b"h":
                        out.extend(HEALTH_REPLY)
                    elif pending_stmt == b"c":
                        out.extend(CREATE_POST_REPLY)
                    elif pending_stmt == b"l":
                        out.extend(LIKE_REPLY)
                elif tag == b"S":
                    out.extend(READY_IDLE)
                elif tag == b"X":
                    return
            if pos > 0:
                del rx[:pos]
            if out:
                conn.sendall(out)
    except OSError:
        pass
    finally:
        conn.close()


def make_jwt(secret: bytes, sub: str, username: str, exp: int) -> str:
    def b64u(raw: bytes) -> str:
        return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")

    h = b64u(b'{"alg":"HS256","typ":"JWT"}')
    p = b64u(
        json.dumps(
            {"sub": sub, "username": username, "exp": exp},
            separators=(",", ":"),
        ).encode()
    )
    signing = f"{h}.{p}".encode("ascii")
    sig = b64u(hmac.new(secret, signing, hashlib.sha256).digest())
    return f"{h}.{p}.{sig}"


def drive_http(port: int, token: str, iterations: int) -> None:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    gz_hdr = {"Accept-Encoding": "gzip"}
    auth_gz = {
        "Accept-Encoding": "gzip",
        "Authorization": f"Bearer {token}",
        "Content-Type": "application/json",
    }
    post_payload = b'{"body":"Training workload sample with \\"quotes\\" and unicode cafe"}'
    for i in range(iterations):
        conn.request("GET", "/feed", headers=gz_hdr if (i & 7) != 0 else {})
        r = conn.getresponse()
        r.read()
        if (i & 1) == 0:
            conn.request("GET", "/posts/42", headers=gz_hdr)
            conn.getresponse().read()
        if (i & 15) == 0:
            conn.request("POST", "/posts/42/like", headers=auth_gz)
            conn.getresponse().read()
        if (i & 31) == 0:
            conn.request("POST", "/posts", body=post_payload, headers=auth_gz)
            conn.getresponse().read()
        if (i & 63) == 0:
            conn.request("GET", "/posts/999999", headers=gz_hdr)
            conn.getresponse().read()
            conn.request("GET", "/health")
            conn.getresponse().read()
    conn.close()


def main() -> None:
    bin_path = sys.argv[1]
    prof_dir = sys.argv[2]

    pg_srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    pg_srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    pg_srv.bind(("127.0.0.1", 0))
    pg_port = pg_srv.getsockname()[1]
    pg_srv.listen(128)
    pg_srv.settimeout(0.2)

    stop_ev = threading.Event()

    def accept_pg() -> None:
        while not stop_ev.is_set():
            try:
                c, _ = pg_srv.accept()
            except TimeoutError:
                continue
            except OSError:
                break
            threading.Thread(target=handle_pg_conn, args=(c, stop_ev), daemon=True).start()

    acc_thread = threading.Thread(target=accept_pg, daemon=True)
    acc_thread.start()

    secret = b"pgo-training-secret"
    tok = make_jwt(secret, "7", "trainer_07", int(time.time()) + 3600)
    modes = [
        {"FEED_REACTOR": "epoll"},
        {"FEED_REACTOR": "uring", "FEED_URING_PBUF_RX": "0", "FEED_DIRECT_FD": "0"},
        {"FEED_REACTOR": "uring", "FEED_URING_PBUF_RX": "1", "FEED_DIRECT_FD": "1"},
    ]

    try:
        for idx_m, mode_env in enumerate(modes):
            probe_s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            probe_s.bind(("127.0.0.1", 0))
            http_port = probe_s.getsockname()[1]
            probe_s2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            probe_s2.bind(("127.0.0.1", 0))
            extra_port = probe_s2.getsockname()[1]
            probe_s.close()
            probe_s2.close()

            env = os.environ.copy()
            env.update(
                {
                    "HOST": "127.0.0.1",
                    "PORT": str(http_port),
                    "EXTRA_PORT": str(extra_port),
                    "DATABASE_URL": f"postgres://feed:feed@127.0.0.1:{pg_port}/feed",
                    "JWT_SECRET": secret.decode("ascii"),
                    "FEED_WORKERS": "1",
                    "FEED_READ_LANES": "8",
                    "FEED_WRITE_LANES": "2",
                    "LLVM_PROFILE_FILE": os.path.join(prof_dir, f"feed-{idx_m}-%p.profraw"),
                }
            )
            env.update(mode_env)

            proc = subprocess.Popen([bin_path], env=env)
            try:
                for _ in range(50):
                    try:
                        hc = http.client.HTTPConnection("127.0.0.1", http_port, timeout=1)
                        hc.request("GET", "/health")
                        resp = hc.getresponse()
                        resp.read()
                        hc.close()
                        if resp.status == 200:
                            break
                    except OSError:
                        pass
                    time.sleep(0.05)

                workers = [
                    threading.Thread(target=drive_http, args=(http_port, tok, 400)),
                    threading.Thread(target=drive_http, args=(http_port, tok, 400)),
                    threading.Thread(target=drive_http, args=(extra_port, tok, 250)),
                    threading.Thread(target=drive_http, args=(extra_port, tok, 250)),
                ]
                for w in workers:
                    w.start()
                time.sleep(1.05)
                for w in workers:
                    w.join()
            finally:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
    finally:
        stop_ev.set()
        pg_srv.close()


if __name__ == "__main__":
    main()
