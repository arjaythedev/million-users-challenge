"""Emit signed test inputs using Python's independent HMAC/SHA-256 implementation."""
import base64
import hashlib
import hmac
import json
import sys

def b64(value):
    return base64.urlsafe_b64encode(value).rstrip(b'=').decode()

def token(header, payload, key=b'round2-test-secret'):
    data = b64(header.encode()) + '.' + b64(payload.encode())
    return data + '.' + b64(hmac.digest(key, data.encode(), hashlib.sha256))

canonical = '{"alg":"HS256","typ":"JWT"}'
claims = dict(sub='7', username='alice', exp=2000000001)
bad = 'invalid or expired token'
payload_error = 'invalid token payload'

def emit(expected, header=canonical, claim=claims, raw=None):
    body = raw if raw is not None else json.dumps(claim, separators=(',', ':'))
    print(expected + '\tBearer ' + token(header, body))

for header in (canonical, '{"typ":"JWT","alg":"HS256"}', '{"alg":"HS256"}',
               '{ "alg" : "HS256", "extra":[1,true] }', '{"alg":"HS384","alg":"HS256"}',
               '{"alg":"HS\\u0032\\u0035\\u0036"}'):
    emit('ok', header)
for header in ('{"alg":"HS384"}', '{"alg":"none"}', '{"alg":"HS256","alg":"HS384"}',
               '{"alg":256}', '{}', 'null', '{"alg":"HS256",}', '{"alg":"HS256"}x'):
    emit(bad, header)
for extra in ({'exp': 2000000000.2}, {'exp': 2000000000.25}, {'exp': 1999999999},
              {'nbf': 2000000000.5}, {'exp': '2000000001'}, {'nbf': '1'}):
    emit(bad, claim=claims | extra)
for extra in ({'exp': 2000000000.5}, {'nbf': 2000000000.25}, {'nbf': 2000000000.1}):
    emit('ok', claim=claims | extra)
for extra in ({'sub': 7}, {'sub': '0'}, {'sub': '-1'}, {'username': 1}):
    emit(payload_error, claim=claims | extra)
for raw in ('{}', '[]', '{"exp":2000000001,}', '{"exp":2000000001}x'):
    emit(bad, raw=raw)
print('missing bearer token\tBasic xxx')
good = token(canonical, json.dumps(claims))
print(bad + '\tBearer ' + good[:-43] + ('A' if good[-43] != 'A' else 'B') + good[-42:])
print(bad + '\tBearer ' + good + '.extra')
