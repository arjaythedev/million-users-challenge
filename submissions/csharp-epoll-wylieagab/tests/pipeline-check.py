#!/usr/bin/env python3
"""Exercise SQL-error recovery through the public API, using only Python's stdlib.

Run against a seeded, running test server:
  BASE_URL=http://127.0.0.1:3000 JWT_SECRET=million-users-challenge \
    python3 tests/pipeline-check.py --concurrency 128

Defaults create 66 uniquely labeled posts. No schema changes, direct SQL, server
fault injection, or automatic request retries are performed. --concurrency controls
client connections, independently of the server's worker count. The synchronized
burst encourages outstanding database queries; black-box HTTP cannot prove exact
pipeline occupancy. The sequential case reuses one verified persistent socket.
"""

import argparse
import base64
import concurrent.futures
import datetime
import gzip
import hashlib
import hmac
import http.client
import json
import os
import re
import secrets
import sys
import threading
import time
import urllib.parse


MISSING_POST = 2**53 - 1
POST_KEYS = ["id", "body", "created_at", "author", "like_count"]
TIMESTAMP = re.compile(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z\Z")


class CheckFailure(Exception):
    pass


def require(condition, message):
    if not condition:
        raise CheckFailure(message)


def json_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON key: " + key)
        result[key] = value
    return result


def reject_constant(value):
    raise CheckFailure("invalid JSON number: " + value)


def make_token(secret, user_id, username):
    def encode(value):
        raw = json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode()
        return base64.urlsafe_b64encode(raw).rstrip(b"=")

    now = int(time.time())
    unsigned = encode({"alg": "HS256", "typ": "JWT"}) + b"." + encode(
        {"sub": str(user_id), "username": username, "iat": now, "exp": now + 3600}
    )
    signature = hmac.new(secret.encode(), unsigned, hashlib.sha256).digest()
    return (unsigned + b"." + base64.urlsafe_b64encode(signature).rstrip(b"=")).decode()


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.counts = {
            "http_requests": 0,
            "expected_sql_errors": 0,
            "created_posts": 0,
            "readbacks": 0,
            "completed_mixed_writes": 0,
        }
        self.ids = set()

    def increment(self, key):
        with self.lock:
            self.counts[key] += 1

    def created(self, post_id):
        with self.lock:
            require(post_id not in self.ids, "duplicate created post ID: " + str(post_id))
            self.ids.add(post_id)
            self.counts["created_posts"] += 1


class Client:
    def __init__(self, args, token, state):
        url = urllib.parse.urlsplit(args.base_url)
        kind = http.client.HTTPSConnection if url.scheme == "https" else http.client.HTTPConnection
        self.connection = kind(url.hostname, url.port, timeout=args.timeout)
        self.token = token
        self.state = state
        self.socket = None

    def __enter__(self):
        self.connection.connect()
        self.socket = self.connection.sock
        return self

    def __exit__(self, *_):
        self.connection.close()

    def request(self, method, path, expected_status, body=None):
        require(self.connection.sock is self.socket, "persistent socket was replaced before request")
        encoded = None if body is None else json.dumps(
            body, separators=(",", ":"), ensure_ascii=False
        ).encode("utf-8")
        self.state.increment("http_requests")
        self.connection.request(method, path, body=encoded, headers={
            "Authorization": "Bearer " + self.token,
            "Accept-Encoding": "identity",
            "Content-Type": "application/json",
            "Connection": "keep-alive",
        })
        response = self.connection.getresponse()
        raw = response.read()
        context = method + " " + path
        require(response.status == expected_status,
                f"{context}: expected {expected_status}, got {response.status}; body={raw[:240]!r}")
        require(self.connection.sock is self.socket and not response.will_close,
                context + ": server did not preserve the persistent connection")
        require(response.getheader("Content-Type", "").split(";", 1)[0].lower() == "application/json",
                context + ": incorrect Content-Type")
        encoding = response.getheader("Content-Encoding", "identity").lower()
        require(encoding in ("identity", "gzip"), context + ": unsupported Content-Encoding")
        if encoding == "gzip":
            raw = gzip.decompress(raw)
        return json.loads(raw.decode("utf-8"), object_pairs_hook=json_object, parse_constant=reject_constant)


def check_health(client):
    result = client.request("GET", "/health", 200)
    require(isinstance(result, dict) and list(result) == ["status", "db", "uptime_s"],
            "health response shape/order mismatch")
    require(result["status"] == "ok" and result["db"] == "ok"
            and type(result["uptime_s"]) is int and result["uptime_s"] >= 0,
            "health response does not report a successful database query")


def missing_like(client, state):
    result = client.request("POST", f"/posts/{MISSING_POST}/like", 404)
    require(result == {"error": "post not found"}, "failed like returned incorrect error")
    state.increment("expected_sql_errors")


def check_post(result, body, author=None):
    require(isinstance(result, dict) and list(result) == ["post"], "post wrapper shape/order mismatch")
    post = result["post"]
    require(isinstance(post, dict) and list(post) == POST_KEYS, "post field shape/order mismatch")
    require(type(post["id"]) is int and post["id"] > 0, "invalid post ID")
    require(post["body"] == body, "post body was associated with the wrong request")
    require(isinstance(post["author"], str), "post author is not a string")
    if author is not None:
        require(post["author"] == author, "post author mismatch")
    require(type(post["like_count"]) is int and post["like_count"] == 0, "new post has incorrect like_count")
    timestamp = post["created_at"]
    require(isinstance(timestamp, str) and TIMESTAMP.fullmatch(timestamp), "incorrect UTC millisecond timestamp")
    datetime.datetime.strptime(timestamp, "%Y-%m-%dT%H:%M:%S.%fZ")
    return post


def create_post(client, state, body, username):
    post = check_post(client.request("POST", "/posts", 201, {"body": body}), body, username)
    state.created(post["id"])
    return post


def read_back(client, state, created, database_author=None):
    post = check_post(client.request("GET", f'/posts/{created["id"]}', 200),
                      created["body"], database_author)
    require(post["id"] == created["id"], "read-back ID mismatch")
    require(post["created_at"] == created["created_at"], "read-back creation time mismatch")
    state.increment("readbacks")
    return post["author"]


def worker(index, args, token, state, barrier, run_id, database_author):
    try:
        with Client(args, token, state) as client:
            # Connect every participant first so the small test produces a burst.
            barrier.wait(timeout=args.timeout)
            for request_index in range(index, args.requests, args.concurrency):
                if request_index % 2 == 0:
                    missing_like(client, state)
                    state.increment("completed_mixed_writes")
                else:
                    body = f'pipeline-{run_id}-{request_index}: quote=" slash=\\ snow=雪 rocket=🚀'
                    post = create_post(client, state, body, args.username)
                    state.increment("completed_mixed_writes")
                    # This reads through a separate database connection immediately
                    # after 201; success must already represent a committed write.
                    read_back(client, state, post, database_author)
        return None
    except Exception as error:
        barrier.abort()
        return f"client {index}: {type(error).__name__}: {error}"


def arguments():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base-url", default=os.environ.get("BASE_URL", "http://127.0.0.1:3000"))
    parser.add_argument("--jwt-secret", default=os.environ.get("JWT_SECRET", "million-users-challenge"))
    parser.add_argument("--user-id", type=int, default=1, help="existing seeded user ID (default: 1)")
    parser.add_argument("--username", default="pipeline-check", help="JWT username; need not equal the database name")
    parser.add_argument("--concurrency", type=int, default=128, help="simultaneous HTTP connections, 1..128 (default: 128)")
    parser.add_argument("--requests", type=int, default=128, help="even mixed-write count, 2..256 (default: 128)")
    parser.add_argument("--timeout", type=float, default=15, help="per-request/start-barrier timeout in seconds (default: 15)")
    args = parser.parse_args()
    url = urllib.parse.urlsplit(args.base_url)
    if (url.scheme not in ("http", "https") or not url.hostname or url.path not in ("", "/")
            or url.query or url.fragment or url.username or url.password):
        parser.error("--base-url must be an http(s) server origin without credentials or a path")
    if not 2 <= args.requests <= 256 or args.requests % 2:
        parser.error("--requests must be an even integer from 2 through 256")
    if not 1 <= args.concurrency <= min(128, args.requests):
        parser.error("--concurrency must be from 1 through min(128, --requests)")
    if not 0 < args.user_id < 2**53 or not 0 < args.timeout <= 120:
        parser.error("--user-id must be a positive integer below 2^53 and --timeout must be in (0,120]")
    return args


def main():
    args = arguments()
    started = time.perf_counter()
    run_id = secrets.token_hex(8)
    token = make_token(args.jwt_secret, args.user_id, args.username)
    state = State()
    errors = []
    try:
        with Client(args, token, state) as client:
            check_health(client)
            require(client.request("GET", f"/posts/{MISSING_POST}", 404) == {"error": "post not found"},
                    "missing-post prerequisite failed")
            # One persistent HTTP socket stays on one reactor worker. Two following
            # successes exercise both write connections after the foreign-key error.
            missing_like(client, state)
            first = create_post(client, state, f"pipeline-{run_id}-sequential-1", args.username)
            second = create_post(client, state, f"pipeline-{run_id}-sequential-2", args.username)
            database_author = read_back(client, state, first)
            read_back(client, state, second, database_author)

        barrier = threading.Barrier(args.concurrency)
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as pool:
            futures = [pool.submit(worker, index, args, token, state, barrier, run_id, database_author)
                       for index in range(args.concurrency)]
            for future in concurrent.futures.as_completed(futures):
                error = future.result()
                if error:
                    errors.append(error)

        with Client(args, token, state) as client:
            check_health(client)
        require(state.counts["completed_mixed_writes"] == args.requests, "not all mixed writes completed")
        require(state.counts["expected_sql_errors"] == 1 + args.requests // 2, "missing expected SQL-error responses")
        require(state.counts["created_posts"] == 2 + args.requests // 2, "missing successful create responses")
        require(state.counts["readbacks"] == state.counts["created_posts"], "not all successful creates were readable")
    except Exception as error:
        errors.append(f"{type(error).__name__}: {error}")

    summary = {
        "test": "postgres-pipeline-error-recovery",
        "passed": not errors,
        "base_url": args.base_url,
        "run_id": run_id,
        "concurrency": args.concurrency,
        "mixed_requests": args.requests,
        **state.counts,
        "duration_s": round(time.perf_counter() - started, 3),
        "error_count": len(errors),
        "errors": errors[:20],
    }
    print(json.dumps(summary, ensure_ascii=True, separators=(",", ":")), flush=True)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
