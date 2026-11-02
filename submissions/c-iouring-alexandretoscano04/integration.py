#!/usr/bin/env python3
import argparse
import base64
import concurrent.futures
import gzip
import hashlib
import hmac
import http.client
import json
import os
import socket
import subprocess
import sys
import time
import urllib.parse
import uuid


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def compact(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode()


class RawHTTP:
    def __init__(self, url):
        parsed = urllib.parse.urlsplit(url)
        self.sock = socket.create_connection((parsed.hostname, parsed.port or 80), 5)
        self.sock.settimeout(5)
        self.reader = self.sock.makefile("rb")

    def response(self):
        status = self.reader.readline(8192).split()
        require(len(status) >= 2, "missing HTTP status line")
        code = int(status[1])
        headers = {}
        while True:
            line = self.reader.readline(8192)
            require(bool(line), "EOF inside headers")
            if line == b"\r\n":
                break
            key, value = line.decode("iso-8859-1").split(":", 1)
            headers[key.lower()] = value.strip()
        body = b""
        if code >= 200 and code not in (204, 304):
            if headers.get("transfer-encoding", "").lower() == "chunked":
                chunks = []
                while True:
                    length = int(self.reader.readline().split(b";", 1)[0], 16)
                    if not length:
                        while True:
                            trailer = self.reader.readline(8192)
                            require(bool(trailer), "EOF inside chunk trailers")
                            if trailer == b"\r\n":
                                break
                        break
                    chunks.append(self.reader.read(length))
                    require(self.reader.read(2) == b"\r\n", "invalid chunk delimiter")
                body = b"".join(chunks)
            else:
                require("content-length" in headers, "response lacks HTTP framing")
                length = int(headers["content-length"])
                body = self.reader.read(length)
                require(len(body) == length, "truncated response body")
        if headers.get("content-encoding") == "gzip":
            body = gzip.decompress(body)
        return code, headers, body

    def close(self):
        self.reader.close()
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class Checks:
    def __init__(self, args):
        self.args = args
        self.passed = 0
        self.failed = 0
        self.secret = os.environ.get("JWT_SECRET", "million-users-challenge").encode()
        self.name = "integration_" + uuid.uuid4().hex
        self.token = self.jwt()

    def sql(self, statement):
        result = subprocess.run(
            [self.args.psql, "-XAt", "-v", "ON_ERROR_STOP=1", "-d", self.args.database],
            input=statement, text=True, capture_output=True, timeout=20,
        )
        require(result.returncode == 0, result.stderr.strip())
        return result.stdout.strip()

    def jwt(self, claims=None, header=None):
        if claims is None:
            claims = {"sub": "1", "username": self.name, "exp": int(time.time()) + 3600}
        encode = lambda data: base64.urlsafe_b64encode(compact(data)).rstrip(b"=")
        unsigned = encode(header or {"alg": "HS256", "typ": "JWT"}) + b"." + encode(claims)
        signature = base64.urlsafe_b64encode(hmac.new(self.secret, unsigned, hashlib.sha256).digest()).rstrip(b"=")
        return (unsigned + b"." + signature).decode()

    def request(self, method, path, body=None, token=None, headers=None, base=None):
        parsed = urllib.parse.urlsplit(base or self.args.base_url)
        connection = http.client.HTTPConnection(parsed.hostname, parsed.port or 80, timeout=8)
        request_headers = dict(headers or {})
        if token is not None:
            request_headers["Authorization"] = "Bearer " + token
        if body is not None:
            request_headers["Content-Type"] = "application/json"
        try:
            connection.request(method, path, body, request_headers)
            response = connection.getresponse()
            data = response.read()
            response_headers = dict((k.lower(), v) for k, v in response.getheaders())
            if response_headers.get("content-encoding") == "gzip":
                data = gzip.decompress(data)
            return response.status, response_headers, json.loads(data)
        finally:
            connection.close()

    def expect(self, result, status, value=None):
        require(result[0] == status, f"expected {status}, got {result}")
        require(result[1].get("content-type", "").startswith("application/json"), "JSON content-type missing")
        if value is not None:
            require(result[2] == value, f"expected {value!r}, got {result[2]!r}")
        return result[2]

    def create(self, body, token=None):
        value = self.expect(self.request("POST", "/posts", compact({"body": body}), token or self.token), 201)
        require(list(value["post"]) == ["id", "body", "created_at", "author", "like_count"], "post key order")
        return value["post"]

    def case(self, name, action):
        try:
            action()
            self.passed += 1
            print("ok", name, flush=True)
        except Exception as error:
            self.failed += 1
            print("FAIL", name, str(error), flush=True)

    def transport(self):
        for path in ["/posts/1/extra/like", "/posts/1/like/like", "/posts//like"]:
            for token in [None, self.token]:
                self.expect(self.request("POST", path, b"{}", token), 404, {"error": "not found"})
        with RawHTTP(self.args.base_url) as client:
            pieces = [b"G", b"ET /hea", b"lth HTTP/1.1\r", b"\nHost: local\r\n", b"\r\n"]
            for piece in pieces:
                client.sock.sendall(piece)
                time.sleep(0.01)
            code, _, body = client.response()
            require(code == 200 and json.loads(body)["db"] == "ok", "fragmented request")
            client.sock.sendall(b"GET /feed HTTP/1.1\r\nHost: local\r\n\r\nGET /missing-integration HTTP/1.1\r\nHost: local\r\n\r\nGET /health HTTP/1.1\r\nHost: local\r\n\r\n")
            responses = [client.response() for _ in range(3)]
            require([r[0] for r in responses] == [200, 404, 200], "pipeline response order")
            require(len(json.loads(responses[0][2])["posts"]) == 20, "pipelined feed body")
            require(json.loads(responses[1][2]) == {"error": "not found"}, "pipelined 404 body")
        with RawHTTP(self.args.base_url) as client:
            client.sock.sendall(b"GET /health HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n")
            require(client.response()[0] == 200, "close response")
            require(client.reader.read(1) == b"", "Connection: close did not close")

    def expect_continue(self):
        expected = self.name + " continue"
        body = compact({"body": expected})
        with RawHTTP(self.args.base_url) as client:
            header = f"POST /posts HTTP/1.1\r\nHost: local\r\nAuthorization: Bearer {self.token}\r\nContent-Type: application/json\r\nContent-Length: {len(body)}\r\nExpect: 100-continue\r\n\r\n"
            client.sock.sendall(header.encode())
            require(client.response()[0] == 100, "missing interim 100 Continue")
            for start in range(0, len(body), 7):
                client.sock.sendall(body[start:start + 7])
            code, _, data = client.response()
            require(code == 201 and json.loads(data)["post"]["body"] == expected, "continued post")

    def encodings(self):
        for accept in ["gzip;q=0", "gzip;q=0, *;q=1", "gzip;q=0.0, identity;q=1"]:
            result = self.request("GET", "/feed", headers={"Accept-Encoding": accept})
            self.expect(result, 200)
            require(result[1].get("content-encoding") != "gzip", "gzip used despite q=0")
        result = self.request("GET", "/feed", headers={"Accept-Encoding": "gzip"})
        self.expect(result, 200)
        require(len(result[2]["posts"]) == 20, "gzip feed decoded")

    def authentication(self):
        good = {"sub": "1", "username": self.name, "exp": int(time.time()) + 3600}
        invalid = ["a.b.c", self.token + ".extra", ".".join(self.token.split(".")[:2]) + ".!!!!"]
        invalid += [self.jwt(good, {"alg": "none"}), self.jwt(good, {"alg": "HS384"})]
        for exp in [int(time.time()) - 1, 0, None]:
            invalid.append(self.jwt(dict(good, exp=exp)))
        missing = dict(good)
        del missing["exp"]
        invalid.append(self.jwt(missing))
        for token in invalid:
            self.expect(self.request("POST", "/posts", b'{"body":"unused"}', token), 401, {"error": "invalid or expired token"})
        for sub in ["0", "-1", "abc", "1.5", 1, None, True]:
            self.expect(self.request("POST", "/posts", b'{"body":"unused"}', self.jwt(dict(good, sub=sub))), 401, {"error": "invalid token payload"})
        for username in [None, 7, [], {}]:
            self.expect(self.request("POST", "/posts", b'{"body":"unused"}', self.jwt(dict(good, username=username))), 401, {"error": "invalid token payload"})
        username = "雪🙂\"\\\n"
        post = self.create(self.name + " Unicode author", self.jwt(dict(good, username=username)))
        require(post["author"] == username, "token username was modified")

    def json_and_unicode(self):
        for data in [b'{"body":"x",}', b'{"body":"x"}junk', b'{"body":NaN}', b'{"body":"\xff"}']:
            self.expect(self.request("POST", "/posts", data, self.token), 400, {"error": "malformed JSON body"})
        for value in [{}, {"body": None}, {"body": 1}, [], None, {"body": " \t\r\n\u00a0\u2003 "}]:
            self.expect(self.request("POST", "/posts", compact(value), self.token), 400, {"error": "body is required"})
        for character in ["é", "🙂"]:
            expected = character * 500
            post = self.create(" \t\r\n\u00a0\u2003" + expected + "\u2003\u00a0\n ")
            require(post["body"] == expected, "Unicode trimming or 500-character limit")
            row = self.sql(f"SELECT encode(convert_to(body,'UTF8'),'hex') FROM posts WHERE id={int(post['id'])};")
            require(row == expected.encode().hex(), "post not committed with exact Unicode body")
            self.expect(self.request("POST", "/posts", compact({"body": expected + character}), self.token), 400, {"error": "body must be at most 500 characters"})

    def concurrent_writes(self):
        post = self.create(self.name + " likes")
        post_id = int(post["id"])
        require(self.sql(f"SELECT count(*) FROM posts WHERE id={post_id};") == "1", "create response preceded commit")
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            responses = list(pool.map(lambda _: self.request("POST", f"/posts/{post_id}/like", token=self.token), range(12)))
        require(sorted(r[0] for r in responses) == [200] * 11 + [201], "concurrent likes must insert exactly once")
        for result in responses:
            self.expect(result, result[0], {"liked": True, "already_liked": result[0] == 200, "post_id": post_id})
        require(self.sql(f"SELECT count(*) FROM likes WHERE post_id={post_id};") == "1", "like response preceded commit or duplicates")
        require(self.expect(self.request("GET", f"/posts/{post_id}"), 200)["post"]["like_count"] == 1, "fresh like count")
        def create_committed(index):
            created = self.create(self.name + f" concurrent {index}")
            require(self.sql(f"SELECT count(*) FROM posts WHERE id={int(created['id'])};") == "1", "concurrent response preceded commit")
            return created["id"]
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            ids = list(pool.map(create_committed, range(8)))
        require(len(set(ids)) == 8, "concurrent creates reused IDs")

    def second_port(self):
        health = self.expect(self.request("GET", "/health", base=self.args.extra_base_url), 200)
        require(health["status"] == "ok", "extra port health")
        require(len(self.expect(self.request("GET", "/feed", base=self.args.extra_base_url), 200)["posts"]) == 20, "extra port feed")

    def idle(self):
        with RawHTTP(self.args.base_url) as client:
            request = b"GET /health HTTP/1.1\r\nHost: local\r\n\r\n"
            client.sock.sendall(request)
            require(client.response()[0] == 200, "pre-idle health")
            time.sleep(self.args.idle_seconds)
            client.sock.sendall(request)
            require(client.response()[0] == 200, "idle connection closed before 75 seconds")

    def recovery(self):
        require(os.environ.get("PGHOST") in ("127.0.0.1", "localhost", "/tmp") and os.environ.get("PGPORT") == "55432", "recovery requires isolated local PostgreSQL on 55432")
        require(self.args.database in ("feed_extra", "feed_bench"), "recovery requires disposable feed_extra/feed_bench")
        name = self.args.application_name.replace("'", "''")
        predicate = f"datname=current_database() AND application_name='{name}' AND usename=current_user AND backend_type='client backend' AND pid<>pg_backend_pid()"
        require(int(self.sql(f"SELECT count(*) FROM pg_stat_activity WHERE {predicate};")) > 0, "no safely identified app sessions; set DATABASE_URL application_name")
        terminated = self.sql(f"SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE {predicate};")
        require("t" in terminated.splitlines(), "no app connections terminated")
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            try:
                result = self.request("GET", "/health")
                if result[0] == 200:
                    self.create(self.name + " recovered")
                    return
            except (AssertionError, OSError, http.client.HTTPException):
                pass
            time.sleep(0.2)
        raise AssertionError("database pool did not reconnect within 20 seconds")

    def run(self):
        require(self.args.database not in ("feed_seed", "postgres", "template0", "template1"), "use a disposable database copy")
        require(self.sql("SELECT current_database();") == self.args.database, "database mismatch")
        for name in ["transport", "expect_continue", "encodings", "authentication", "json_and_unicode", "concurrent_writes", "second_port"]:
            self.case(name, getattr(self, name))
        if self.args.idle_seconds:
            self.case("idle_connection", self.idle)
        if self.args.terminate_db_connections:
            self.case("database_connection_recovery", self.recovery)
        print(json.dumps({"passed": self.passed, "failed": self.failed, "database": self.args.database}))
        return bool(self.failed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default=os.environ.get("BASE_URL", "http://127.0.0.1:18080"))
    parser.add_argument("--extra-base-url", default=os.environ.get("EXTRA_BASE_URL", "http://127.0.0.1:18081"))
    parser.add_argument("--database", default=os.environ.get("PGDATABASE", "feed_extra"))
    parser.add_argument("--psql", default="psql")
    parser.add_argument("--idle-seconds", type=float, default=0, help="75.2 verifies the minimum idle lifetime")
    parser.add_argument("--terminate-db-connections", action="store_true", help="terminate only named app sessions in local disposable DB")
    parser.add_argument("--application-name", default="compact-integration")
    args = parser.parse_args()
    for url in [args.base_url, args.extra_base_url]:
        require(urllib.parse.urlsplit(url).scheme == "http", "only plain HTTP is supported")
    return Checks(args).run()


if __name__ == "__main__":
    sys.exit(main())
