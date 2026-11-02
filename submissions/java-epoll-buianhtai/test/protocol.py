#!/usr/bin/env python3
"""Consumer-visible protocol and commit invariants. Run against a running throwaway database/server."""
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
import time
import unittest
import uuid

HOST = os.environ.get('TEST_HOST', '127.0.0.1')
PORT = int(os.environ.get('PORT', '3000'))
SECRET = os.environ.get('JWT_SECRET', 'million-users-challenge').encode()


def token(claims=None):
    def b64(b):
        return base64.urlsafe_b64encode(b).rstrip(b'=')
    payload = claims or json.dumps({'sub': '1', 'username': 'golden_ember_1', 'exp': time.time() + 3600})
    signed = b64(b'{"alg":"HS256"}') + b'.' + b64(payload.encode())
    return (signed + b'.' + b64(hmac.digest(SECRET, signed, hashlib.sha256))).decode()


def request(path, body=None, auth=None, encoding=None):
    c = http.client.HTTPConnection(HOST, PORT, timeout=20)
    headers = {}
    if auth is not None:
        headers['Authorization'] = 'Bearer ' + auth
    if encoding:
        headers['Accept-Encoding'] = encoding
    try:
        c.request('GET' if body is None else 'POST', path, body, headers)
        r = c.getresponse()
        data = r.read()
        if r.getheader('content-encoding') == 'gzip':
            data = gzip.decompress(data)
        return r.status, json.loads(data)
    finally:
        c.close()


class Protocol(unittest.TestCase):
    def test_rejects_invalid_json_numbers(self):
        for number in ('01', '+1', '1.', '.1', '1e', '--1'):
            with self.subTest(number=number):
                status, body = request('/posts', '{"body":"not inserted","extra":' + number + '}', token())
                self.assertEqual((status, body), (400, {'error': 'malformed JSON body'}))

    def test_rejects_nonfinite_token_expiration(self):
        t = token('{"sub":"1","username":"golden_ember_1","exp":1e999}')
        self.assertEqual(request('/posts', '{"body":"not inserted"}', t),
                         (401, {'error': 'invalid or expired token'}))

    def test_fragmented_and_pipelined_requests_keep_order(self):
        with socket.create_connection((HOST, PORT), timeout=20) as s:
            s.sendall(b'GET /posts/1 HTTP/1.1\r\nHost: x\r\n')
            time.sleep(0.02)
            s.sendall(b'\r\nGET /posts/250000 HTTP/1.1\r\nHost: x\r\n\r\n'
                      b'GET /posts/500000 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n')
            f = s.makefile('rb')
            for expected in (1, 250000, 500000):
                self.assertEqual(f.readline().strip(), b'HTTP/1.1 200 OK')
                headers = {}
                while (line := f.readline()) != b'\r\n':
                    self.assertTrue(line)
                    k, v = line.decode().split(':', 1)
                    headers[k.lower()] = v.strip()
                data = json.loads(f.read(int(headers['content-length'])))
                self.assertEqual(data['post']['id'], expected)
            self.assertEqual(f.read(1), b'')

    def test_gzip_is_decodable_and_matches_plain_response(self):
        self.assertEqual(request('/feed', encoding='gzip'), request('/feed'))

    def test_slow_reader_receives_pipeline_and_final_error_before_close(self):
        with socket.create_connection((HOST, PORT), timeout=30) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16384)
            batch = b'GET /feed HTTP/1.1\r\nHost: x\r\n\r\n' * 1200
            batch += b'POST /posts HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n'
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as ex:
                sender = ex.submit(s.sendall, batch)
                time.sleep(0.1)
                f = s.makefile('rb')
                for i in range(1201):
                    expected = 200 if i < 1200 else 400
                    line = f.readline()
                    self.assertTrue(line.startswith(('HTTP/1.1 ' + str(expected)).encode()), line)
                    headers = {}
                    while (line := f.readline()) != b'\r\n':
                        self.assertTrue(line)
                        k, v = line.decode().split(':', 1)
                        headers[k.lower()] = v.strip()
                    data = json.loads(f.read(int(headers['content-length'])))
                    if expected == 200:
                        self.assertEqual(len(data['posts']), 20)
                    else:
                        self.assertIn('error', data)
                sender.result()
                self.assertEqual(f.read(1), b'')

    def test_concurrent_failed_writes_do_not_lose_successful_commits(self):
        prefix = 'protocol-' + uuid.uuid4().hex
        auth = token()
        def write(i):
            if i % 4 == 0:
                return i, request('/posts/999999999/like', '', auth)
            return i, request('/posts', json.dumps({'body': prefix + '-' + str(i)}), auth)
        with concurrent.futures.ThreadPoolExecutor(max_workers=64) as ex:
            results = list(ex.map(write, range(64)))
        for i, (status, body) in results:
            if i % 4 == 0:
                self.assertEqual((status, body), (404, {'error': 'post not found'}))
            else:
                self.assertEqual(status, 201)
                saved_status, saved = request('/posts/' + str(body['post']['id']))
                self.assertEqual((saved_status, saved), (200, body))
        sql = "SELECT count(*) FROM posts WHERE body LIKE '" + prefix + "-%'"
        count = subprocess.check_output(['psql', '-qXAt', '-d', 'feed_test', '-c', sql], text=True).strip()
        self.assertEqual(count, '48')


if __name__ == '__main__':
    unittest.main(verbosity=2)
