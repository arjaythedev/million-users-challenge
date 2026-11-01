#!/usr/bin/env python3
"""Exercise fragmented requests, large headers and UTF-8 bodies over keep-alive."""
import argparse
import base64
from contextlib import closing
import hashlib
import hmac
import http.client
import json
import os
import socket
import struct
import time
from urllib.parse import urlsplit

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('base', nargs='?', default='http://127.0.0.1:4101')
base = urlsplit(parser.parse_args().base)

def encoded(value):
    return base64.urlsafe_b64encode(value).rstrip(b'=')

signed = encoded(b'{"alg":"HS256","typ":"JWT"}') + b'.' + encoded(json.dumps({
    'sub': '1', 'username': 'golden_ember_1', 'exp': int(time.time()) + 3600,
}).encode())
token = signed + b'.' + encoded(hmac.new(
    os.environ.get('JWT_SECRET', 'million-users-challenge').encode(), signed, hashlib.sha256,
).digest())

with socket.create_connection((base.hostname, base.port), timeout=10) as connection:
    def exchange(request, fragments=False):
        if fragments:
            for offset in range(0, len(request), 137):
                connection.sendall(request[offset:offset + 137])
                time.sleep(.001)
        else:
            connection.sendall(request)
        response = http.client.HTTPResponse(connection)
        response.begin()
        body = response.read()
        status = response.status
        response.close()
        assert status == 200, (status, body)
        assert json.loads(body)['post']['id'] == 1

    # Complete requests exactly filling a buffer must dispatch before it grows.
    prefix = b'GET /posts/1 HTTP/1.1\r\nHost: localhost\r\nX-Padding: '
    for size in (512, 1024, 2048, 4096, 8192, 16384):
        request = prefix + b'x' * (size - len(prefix) - 4) + b'\r\n\r\n'
        exchange(request)
    exchange(prefix + b'x' * 12000 + b'\r\n\r\n', fragments=True)

    # A permitted 500-character body needs more than one small input buffer.
    text = '\U0001f642' * 500
    body = json.dumps({'body': text}, ensure_ascii=False).encode()
    request = (b'POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer '
               + token + b'\r\nContent-Type: application/json\r\nContent-Length: '
               + str(len(body)).encode() + b'\r\n\r\n' + body)
    for offset in range(0, len(request), 137):
        connection.sendall(request[offset:offset + 137])
        time.sleep(.001)
    response = http.client.HTTPResponse(connection)
    response.begin()
    created = response.read()
    assert response.status == 201, (response.status, created)
    assert json.loads(created)['post']['body'] == text
    response.close()
    exchange(b'GET /posts/1 HTTP/1.1\r\nHost: localhost\r\n\r\n')

with socket.create_connection((base.hostname, base.port), timeout=10) as connection:
    connection.sendall(prefix + b'x' * 17000 + b'\r\n\r\n')
    try:
        assert connection.recv(1) == b'', 'oversized request was accepted'
    except ConnectionResetError:
        pass

# Resets can race database completions; later requests must still work.
for _ in range(512):
    with socket.create_connection((base.hostname, base.port), timeout=10) as connection:
        connection.sendall(b'GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\n')
        connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
with closing(http.client.HTTPConnection(base.hostname, base.port, timeout=10)) as connection:
    connection.request('GET', '/posts/1')
    response = connection.getresponse()
    assert response.status == 200
    assert json.loads(response.read())['post']['id'] == 1

print('PASS: buffer boundaries, fragmented headers/body, UTF-8, keep-alive, request limit and resets')
