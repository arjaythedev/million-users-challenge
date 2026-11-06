"""The C path against the Python path it stands in for.

The pure parts always run. The rest needs a database, so it runs only with FEED_TEST_DATABASE_URL
set: it starts the fast and raw servers on one database and requires the same bytes from both, whatever
the input, then checks under concurrency that every response answers the request it should.
"""
import base64
import gzip
import hashlib
import hmac
import json
import os
import random
import socket
import subprocess
import sys
import threading
import time
import unittest
from datetime import UTC, datetime, timedelta
from pathlib import Path

from feed.adapters.outbound.postgres._pq import Loop, iso_millis, time_from_micros

ROOT = Path(__file__).resolve().parent.parent
DATABASE_URL = os.environ.get("FEED_TEST_DATABASE_URL")
SECRET = "conformance-secret"
EPOCH_2000 = datetime(2000, 1, 1, tzinfo=UTC)
HOSTILE_BODIES = [
    'quotes " and \\ backslashes',
    "controls \t tab \b back \f feed \r\n newline \x01 \x1f end",
    "non-ascii é 漢字 😀 \u2028 \u00a0 inside",
    "</script> & <b>",
    "x" * 500,
]


class Pure(unittest.TestCase):
    def test_a_connection_is_a_small_slot(self):
        self.assertLessEqual(Loop.slot_size(), 40)

    def test_timestamps_match_the_text_decoder(self):
        rng = random.Random(7)
        cases = [0, 1, 999, 1000, 999_999, 1_000_000, -1, -1000, -1_000_000, 86_400_000_000 * 59]
        cases += [rng.randrange(-10**15, 2 * 10**15) for _ in range(2000)]
        for us in cases:
            at = EPOCH_2000 + timedelta(microseconds=us)
            text = at.strftime("%Y-%m-%d %H:%M:%S.%f") + "+00"
            self.assertEqual(time_from_micros(us), iso_millis(text), (us, text))


def token(user_id=1, username="alice", exp=None):
    def b64(raw):
        return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()
    head = b64(b'{"alg":"HS256","typ":"JWT"}')
    claims = {"sub": str(user_id), "username": username, "exp": exp or int(time.time()) + 3600}
    body = b64(json.dumps(claims).encode())
    signature = hmac.new(SECRET.encode(), f"{head}.{body}".encode(), hashlib.sha256).digest()
    return f"{head}.{body}.{b64(signature)}"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Client:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.buffer = b""

    def send(self, raw):
        self.sock.sendall(raw)

    def read(self):
        data = self.buffer
        while b"\r\n\r\n" not in data:
            chunk = self.sock.recv(65536)
            if not chunk:
                self.buffer = b""
                return None
            data += chunk
        head, _, rest = data.partition(b"\r\n\r\n")
        lines = head.split(b"\r\n")
        headers = dict(line.split(b": ", 1) for line in lines[1:])
        length = int(headers[b"Content-Length"])
        while len(rest) < length:
            chunk = self.sock.recv(65536)
            if not chunk:
                break
            rest += chunk
        self.buffer = rest[length:]
        return lines[0], headers, rest[:length]

    def close(self):
        self.sock.close()


def get(path, extra=""):
    return f"GET {path} HTTP/1.1\r\nHost: x\r\n{extra}\r\n".encode()


def post(path, body=b"", auth=None):
    bearer = f"Authorization: Bearer {auth}\r\n" if auth else ""
    return f"POST {path} HTTP/1.1\r\nHost: x\r\n{bearer}Content-Length: {len(body)}\r\n\r\n".encode() + body


@unittest.skipUnless(DATABASE_URL, "needs FEED_TEST_DATABASE_URL")
class AgainstPython(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ports = {}
        cls.servers = []
        for name in ("fast", "raw"):
            port = free_port()
            env = {**os.environ, "DATABASE_URL": DATABASE_URL, "JWT_SECRET": SECRET, "HOST": "127.0.0.1",
                   "PORT": str(port), "FEED_SERVER": name, "FEED_WORKERS": "2",
                   "FEED_READ_LANES": "3", "FEED_WRITE_LANES": "2"}
            env.pop("EXTRA_PORT", None)
            cls.servers.append(subprocess.Popen([sys.executable, "-m", "feed"], cwd=ROOT, env=env,
                                                stderr=subprocess.DEVNULL))
            cls.ports[name] = port
        for port in cls.ports.values():
            cls._wait(port)
        client = Client(cls.ports["raw"])
        cls.posts = []
        for body in HOSTILE_BODIES:
            client.send(post("/posts", json.dumps({"body": body}).encode(), token()))
            status, _, raw = client.read()
            assert status.endswith(b"201 Created"), (status, raw)
            cls.posts.append(json.loads(raw)["post"]["id"])
        client.close()

    @classmethod
    def tearDownClass(cls):
        for server in cls.servers:
            server.terminate()
        for server in cls.servers:
            server.wait()

    @staticmethod
    def _wait(port):
        for _ in range(100):
            try:
                client = Client(port)
                client.send(get("/health"))
                if client.read()[0].endswith(b"200 OK"):
                    client.close()
                    return
            except OSError:
                pass
            time.sleep(0.1)
        raise AssertionError(f"server on {port} did not start")

    def exchange(self, name, raw, responses=1):
        client = Client(self.ports[name])
        client.send(raw)
        out = [client.read() for _ in range(responses)]
        client.close()
        return out

    def assert_same(self, raw, responses=1):
        fast = self.exchange("fast", raw, responses)
        python = self.exchange("raw", raw, responses)
        self.assertEqual(fast, python, raw[:200])
        return fast

    def test_feed_is_identical(self):
        status, headers, body = self.assert_same(get("/feed"))[0]
        self.assertEqual(status, b"HTTP/1.1 200 OK")
        posts = json.loads(body)["posts"]
        self.assertEqual(len(posts), 20)
        self.assertEqual({p["body"] for p in posts} & set(HOSTILE_BODIES), set(HOSTILE_BODIES))

    def test_gzipped_feed_decodes_to_the_same_body(self):
        plain = self.exchange("fast", get("/feed"))[0][2]
        for name in ("fast", "raw"):
            status, headers, body = self.exchange(name, get("/feed", "Accept-Encoding: gzip\r\n"))[0]
            self.assertEqual(status, b"HTTP/1.1 200 OK")
            self.assertEqual(headers[b"Content-Encoding"], b"gzip")
            self.assertEqual(headers[b"Vary"], b"Accept-Encoding")
            self.assertEqual(gzip.decompress(body), plain)

    def test_every_hostile_post_is_identical(self):
        for post_id in self.posts:
            status, _, body = self.assert_same(get(f"/posts/{post_id}"))[0]
            self.assertEqual(status, b"HTTP/1.1 200 OK")
            self.assertIn(json.loads(body)["post"]["body"], HOSTILE_BODIES)

    def test_odd_input_is_identical(self):
        paths = ["/posts/1", "/posts/999999999999", "/posts/0", "/posts/00001", "/posts/-1", "/posts/1.5", "/posts/abc",
                 "/posts/", "/posts", "/posts/1/", "/posts/1/x", "/posts/%31", "/posts/١",
                 "/posts/9223372036854775807", "/posts/9223372036854775808", "/posts/" + "9" * 40,
                 "/posts/999999999999999999", "/posts/1?x=1", "/feed?x=1", "/feed/", "/FEED", "/", "/nope"]
        for path in paths:
            self.assert_same(get(path).decode().encode())
        for raw in (b"PUT /feed HTTP/1.1\r\nHost: x\r\n\r\n", b"POST /feed HTTP/1.1\r\nContent-Length: 0\r\n\r\n",
                    b"HEAD /feed HTTP/1.1\r\n\r\n", b"GET /feed HTTP/1.1\r\nCONTENT-LENGTH: 2\r\n\r\nxx"):
            self.assert_same(raw)

    def test_auth_and_write_errors_are_identical(self):
        cases = [post("/posts", b'{"body":"x"}'), post("/posts", b'{"body":"x"}', "not.a.token"),
                 post("/posts", b'{"body":"x"}', token(exp=1)), post("/posts", b"not json", token()),
                 post("/posts", b'{"body":""}', token()), post("/posts", json.dumps({"body": "x" * 501}).encode(), token()),
                 post("/posts/abc/like"), post("/posts/999999999999/like", auth=token()),
                 post("/posts/abc/like", auth=token())]
        for raw in cases:
            self.assert_same(raw)

    def test_connection_close_is_honoured(self):
        for extra in ("Connection: close\r\n", "connection: Close\r\n"):
            client = Client(self.ports["fast"])
            client.send(get("/feed", extra))
            self.assertEqual(client.read()[0], b"HTTP/1.1 200 OK")
            self.assertEqual(client.sock.recv(1), b"")
            client.close()
        self.assertEqual(self.exchange("fast", get("/feed", "Connection: close\r\n")),
                         self.exchange("raw", get("/feed", "Connection: close\r\n")))

    def test_like_on_a_missing_post_is_a_404_and_others_in_its_batch_succeed(self):
        for name in ("fast", "raw"):
            clients = [Client(self.ports[name]) for _ in range(12)]
            for i, client in enumerate(clients):
                missing = i % 3 == 0
                post_id = 999_999_999_000 + i if missing else self.posts[0]
                client.send(post(f"/posts/{post_id}/like", auth=token(user_id=10 + i)))
            for i, client in enumerate(clients):
                status = client.read()[0]
                if i % 3 == 0:
                    self.assertEqual(status, b"HTTP/1.1 404 Not Found")
                else:
                    self.assertIn(status, (b"HTTP/1.1 201 Created", b"HTTP/1.1 200 OK"))
                client.close()

    def test_pipelined_mixed_requests_answer_in_order(self):
        ids = [self.posts[i % len(self.posts)] for i in range(40)]
        raw, want = b"", []
        for i, post_id in enumerate(ids):
            kind = i % 4
            if kind == 0:
                raw += get("/feed")
                want.append(("feed", None))
            elif kind == 1:
                raw += get(f"/posts/{post_id}")
                want.append(("post", post_id))
            elif kind == 2:
                raw += get("/posts/abc")
                want.append(("bad", None))
            else:
                raw += post(f"/posts/{post_id}/like", auth=token(user_id=100 + i))
                want.append(("like", post_id))
        for name in ("fast", "raw"):
            client = Client(self.ports[name])
            client.send(raw)
            for kind, post_id in want:
                status, _, body = client.read()
                document = json.loads(body)
                if kind == "feed":
                    self.assertEqual(len(document["posts"]), 20)
                elif kind == "post":
                    self.assertEqual(document["post"]["id"], post_id)
                elif kind == "bad":
                    self.assertEqual(status, b"HTTP/1.1 400 Bad Request")
                else:
                    self.assertEqual(document["post_id"], post_id)
            client.close()

    def test_a_request_split_byte_by_byte(self):
        raw = get(f"/posts/{self.posts[0]}", "Accept-Encoding: identity\r\n")
        want = self.exchange("raw", raw)[0]
        client = Client(self.ports["fast"])
        client.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        for i in range(len(raw)):
            client.send(raw[i:i + 1])
            if i % 8 == 0:
                time.sleep(0.001)
        self.assertEqual(client.read(), want)
        body = post("/posts", json.dumps({"body": "split"}).encode(), token())
        cut = len(body) - 5
        client.send(body[:cut])
        time.sleep(0.02)
        client.send(body[cut:])
        self.assertEqual(client.read()[0], b"HTTP/1.1 201 Created")
        client.close()

    def test_oversized_requests_drop_the_connection(self):
        for raw in (b"GET /feed HTTP/1.1\r\nX: " + b"y" * 20000,
                    b"POST /posts HTTP/1.1\r\nContent-Length: 99999\r\n\r\n"):
            client = Client(self.ports["fast"])
            client.send(raw)
            self.assertIsNone(client.read())
            client.close()

    def test_every_response_answers_its_own_request_under_load(self):
        """Many connections, pipelined and not, some closing mid-request: no response ever crosses over."""
        failures, answered = [], [0]
        lock = threading.Lock()

        def worker(seed):
            rng = random.Random(seed)
            client = Client(self.ports["fast"])
            try:
                for _ in range(150):
                    ids = [rng.choice(self.posts + [1, 2, 3, 4, 5]) for _ in range(rng.randint(1, 8))]
                    client.send(b"".join(get(f"/posts/{i}") for i in ids))
                    if rng.random() < 0.03:  # gone with its requests in flight: nothing may reach the next owner of its fd
                        client.close()
                        client = Client(self.ports["fast"])
                        continue
                    for i in ids:
                        reply = client.read()
                        document = json.loads(reply[2])
                        if document.get("post", {}).get("id") != i:
                            failures.append((i, reply[2][:100]))
                    with lock:
                        answered[0] += len(ids)
            except Exception as exc:
                failures.append(repr(exc))
            finally:
                client.close()

        threads = [threading.Thread(target=worker, args=(seed,)) for seed in range(40)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(failures, [])
        self.assertGreater(answered[0], 10_000)


if __name__ == "__main__":
    unittest.main()
