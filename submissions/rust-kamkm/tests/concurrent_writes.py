#!/usr/bin/env python3
"""Exercise write-batch rollback, duplicate likes, and commit-before-response."""
import argparse
import base64
from concurrent.futures import ThreadPoolExecutor
import hashlib
import hmac
import http.client
import json
import os
import subprocess
import threading
import time
from urllib.parse import urlsplit

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('base')
parser.add_argument('--database')
parser.add_argument('--docker')
args = parser.parse_args()
base = urlsplit(args.base)
secret = os.environ.get('JWT_SECRET', 'million-users-challenge').encode()
def b64(value):
    return base64.urlsafe_b64encode(value).rstrip(b'=')
signed = b64(b'{"alg":"HS256","typ":"JWT"}') + b'.' + b64(json.dumps({'sub':'1','username':'golden_ember_1','exp':int(time.time())+3600}).encode())
token = (signed + b'.' + b64(hmac.new(secret, signed, hashlib.sha256).digest())).decode()

def request(method, path, body=None):
    connection = http.client.HTTPConnection(base.hostname, base.port, timeout=15)
    headers = {'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'}
    connection.request(method, path, None if body is None else json.dumps(body), headers)
    response = connection.getresponse()
    result = response.status, json.loads(response.read())
    connection.close()
    return result

prefix = 'batch-regression-' + str(time.time_ns())
status, data = request('POST', '/posts', {'body':prefix+'-like-target'})
assert status == 201, (status, data)
post_id = data['post']['id']
barrier = threading.Barrier(32)
def work(i):
    barrier.wait(timeout=10)
    if i % 4 == 0:
        status, data = request('POST', '/posts/999999999/like')
        assert status == 404, (status, data)
        return ('missing', status)
    if i % 4 == 1:
        status, data = request('POST', f'/posts/{post_id}/like')
        assert status in (200, 201), (status, data)
        return ('like', status)
    body = prefix+'-'+str(i)
    status, data = request('POST', '/posts', {'body':body})
    assert status == 201, (status, data)
    # A separate HTTP connection must observe the committed row immediately.
    read_status, read = request('GET', '/posts/'+str(data['post']['id']))
    assert read_status == 200 and read['post']['body'] == body, (read_status, read)
    return ('created', data['post']['id'])
with ThreadPoolExecutor(max_workers=32) as executor:
    results = list(executor.map(work, range(96)))
likes = [status for kind, status in results if kind == 'like']
assert likes.count(201) == 1 and likes.count(200) == 23, likes
created = [value for kind, value in results if kind == 'created']
assert len(set(created)) == 48, created
status, data = request('GET', f'/posts/{post_id}')
assert status == 200 and data['post']['like_count'] == 1, data
if args.database:
    command = ['docker','exec',args.docker,'psql','-U','postgres'] if args.docker else ['psql']
    command += ['-XAt','-v','ON_ERROR_STOP=1','-d',args.database,'-c',f"SELECT count(*) FROM posts WHERE body LIKE '{prefix}%'" ]
    count = int(subprocess.check_output(command, text=True).strip())
    assert count == 49, f'batch retry duplicated committed posts: expected 49, got {count}'
print('PASS: 96 concurrent writes, failed-batch isolation, duplicate likes, immediate visibility, no duplicate posts')
