#!/usr/bin/env python3
"""Exercise API validation and ambiguous HTTP framing through real sockets."""
import argparse
import base64
import hashlib
import hmac
import http.client
import json
import os
import socket
import time
from urllib.parse import urlsplit

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('base')
args = parser.parse_args()
base = urlsplit(args.base)
secret = os.environ.get('JWT_SECRET', 'million-users-challenge').encode()
failures = []


def token(**overrides):
    claims = {'sub': '1', 'username': 'golden_ember_1', 'exp': int(time.time()) + 3600}
    claims.update(overrides)
    encode = lambda b: base64.urlsafe_b64encode(b).rstrip(b'=')
    signed = encode(b'{"alg":"HS256","typ":"JWT"}') + b'.' + encode(json.dumps(claims).encode())
    return (signed + b'.' + encode(hmac.new(secret, signed, hashlib.sha256).digest())).decode()


def request(method, path, body=None, auth=None):
    conn = http.client.HTTPConnection(base.hostname, base.port, timeout=10)
    headers = {'Content-Type': 'application/json'}
    if auth is not None:
        headers['Authorization'] = 'Bearer ' + auth
    try:
        conn.request(method, path, body, headers)
        result = conn.getresponse()
        return result.status, json.loads(result.read())
    finally:
        conn.close()


def check(name, callback):
    try:
        callback()
        print('PASS:', name, flush=True)
    except Exception as error:
        failures.append(name)
        print('FAIL:', name, repr(error), flush=True)


def expect(method, path, status, body, raw=None, auth=None):
    actual = request(method, path, raw, auth)
    assert actual == (status, body), actual


for method, path in [('GET', '/posts/1/extra'), ('GET', '/posts/'),
                     ('POST', '/posts//like'), ('POST', '/posts/1/extra/like')]:
    check(method + ' ' + path, lambda m=method, p=path: expect(m, p, 404, {'error': 'not found'}))

check('authentication before invalid id', lambda: expect(
    'POST', '/posts/abc/like', 401, {'error': 'missing bearer token'}))

for username in [None, False, 123, [], {}]:
    check('username must be a string: ' + repr(username), lambda value=username: expect(
        'POST', '/posts', 401, {'error': 'invalid token payload'},
        '{"body":"rejected token payload"}', token(username=value)))

check('expiration has no grace period', lambda: expect(
    'POST', '/posts', 401, {'error': 'invalid or expired token'},
    '{"body":"expired token"}', token(exp=int(time.time()) - 1)))

check('missing token user is not a missing post', lambda: expect(
    'POST', '/posts/1/like', 500, {'error': 'internal server error'},
    auth=token(sub='999999999')))

for raw, message in [('[]', 'body is required'), ('null', 'body is required'),
                     ('{"body":null}', 'body is required'), ('{"body":false}', 'body is required'),
                     ('{"body":"x"} {}', 'malformed JSON body')]:
    check('body validation: ' + raw, lambda data=raw, msg=message: expect(
        'POST', '/posts', 400, {'error': msg}, data, token()))


def unicode_body():
    text = '\u2003' + '\u00e9' * 500 + '\u2003'
    status, body = request('POST', '/posts', json.dumps({'body': text}), token())
    assert status == 201 and body['post']['body'] == '\u00e9' * 500, (status, body)
    assert body['post']['author'] == 'golden_ember_1', body


check('trim Unicode whitespace and count characters', unicode_body)

check('embedded NUL must not truncate a stored post', lambda: expect(
    'POST', '/posts', 500, {'error': 'internal server error'},
    json.dumps({'body': 'before\u0000after'}), token()))
check('invalid UTF-8 is malformed JSON', lambda: expect(
    'POST', '/posts', 400, {'error': 'malformed JSON body'},
    b'{"body":"\xff"}', token()))


def malformed(raw):
    with socket.create_connection((base.hostname, base.port), timeout=5) as conn:
        conn.sendall(raw)
        with conn.makefile('rb') as reader:
            line = reader.readline()
            assert line.startswith(b'HTTP/1.1 400 '), line


def valid_framing(raw):
    with socket.create_connection((base.hostname, base.port), timeout=5) as conn:
        conn.sendall(raw)
        with conn.makefile('rb') as reader:
            line = reader.readline()
            assert line.startswith(b'HTTP/1.1 401 '), line


check('optional whitespace in content length', lambda: valid_framing(
    b'POST /posts HTTP/1.1\r\nHost: a\r\nContent-Length:\t2 \t\r\n\r\n{}'))
check('optional whitespace in transfer encoding', lambda: valid_framing(
    b'POST /posts HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: Chunked \t\r\n\r\n0\r\n\r\n'))
check('identical content lengths agree', lambda: valid_framing(
    b'POST /posts HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n{}'))


bad_requests = {
    'missing Host header': b'GET /feed HTTP/1.1\r\n\r\n',
    'conflicting content lengths': b'POST /posts HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\nContent-Length: 0\r\n\r\n{}',
    'signed content length': b'POST /posts HTTP/1.1\r\nHost: a\r\nContent-Length: +2\r\n\r\n{}',
    'chunked plus content length': b'POST /posts HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n',
    'repeated transfer encoding': b'POST /posts HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n',
    'signed chunk size': b'POST /posts HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n+1\r\nx\r\n0\r\n\r\n',
    'duplicate Host headers': b'GET /feed HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n',
    'control byte in target': b'GET /feed\x00 HTTP/1.1\r\nHost: a\r\n\r\n',
    'control byte in header': b'GET /feed HTTP/1.1\r\nHost: a\r\nX-Test: bad\x00value\r\n\r\n',
}
for name, raw in bad_requests.items():
    check(name, lambda data=raw: malformed(data))

if failures:
    raise SystemExit('Failed contract checks: ' + ', '.join(failures))
print('PASS: API edge cases and HTTP framing')
