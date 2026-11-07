#!/usr/bin/env python3
"""A successful like must be visible in the very next feed response."""
import argparse
import base64
import hashlib
import hmac
import http.client
import json
import os
import subprocess
import sys
import time
from urllib.parse import urlsplit

def encoded(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=")

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('base', nargs='?', default='http://127.0.0.1:4101')
parser.add_argument('--database', help='also test a commit from another database client')
parser.add_argument('--docker', help='run psql inside this PostgreSQL container')
args = parser.parse_args()
base = urlsplit(args.base)
connection = http.client.HTTPConnection(base.hostname, base.port, timeout=5)
header = encoded(b'{"alg":"HS256","typ":"JWT"}')
payload = encoded(json.dumps({"sub": "1", "username": "golden_ember_1", "exp": int(time.time()) + 3600}).encode())
signed = header + b"." + payload
signature = encoded(hmac.new(os.environ.get("JWT_SECRET", "million-users-challenge").encode(), signed, hashlib.sha256).digest())
token = (signed + b"." + signature).decode()

def request(method, path, body=None):
    headers = {"Authorization": "Bearer " + token}
    if body is not None:
        headers["Content-Type"] = "application/json"
    connection.request(method, path, body=None if body is None else json.dumps(body), headers=headers)
    response = connection.getresponse()
    data = json.loads(response.read())
    assert response.status < 400, (method, path, response.status, data)
    return data

post = request("POST", "/posts", {"body": "feed freshness regression"})["post"]
post_id = post["id"]
before = next(p for p in request("GET", "/feed")["posts"] if p["id"] == post_id)
assert before["like_count"] == 0, before
start = time.monotonic()
if args.database:
    command = ['docker', 'exec', args.docker, 'psql', '-U', 'postgres'] if args.docker else ['psql']
    command += ['-Xq', '-v', 'ON_ERROR_STOP=1', '-d', args.database, '-c', f'INSERT INTO likes (user_id, post_id) VALUES (1, {int(post_id)})']
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
else:
    request("POST", f"/posts/{post_id}/like")
after = next(p for p in request("GET", "/feed")["posts"] if p["id"] == post_id)
assert after["like_count"] == 1, f"stale feed after committed like: wanted 1, got {after['like_count']}"
print(f"PASS: committed like visible in next feed ({(time.monotonic() - start) * 1000:.1f} ms)")
connection.close()
