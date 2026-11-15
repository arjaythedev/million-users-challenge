#!/usr/bin/env python3
"""Verify compressed responses match plain responses across compressor reuse."""
import argparse
import gzip
import http.client
import json
from urllib.parse import urlsplit

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('base')
args = parser.parse_args()
base = urlsplit(args.base)
conn = http.client.HTTPConnection(base.hostname, base.port, timeout=15)

try:
    for _ in range(8):
        conn.request('GET', '/feed', headers={'Accept-Encoding': 'identity'})
        plain = conn.getresponse()
        assert plain.status == 200, plain.status
        expected = plain.read()
        assert plain.getheader('Content-Encoding') is None
        assert len(expected) >= 1024, 'Use a fresh seeded database to exercise compression'
        assert len(json.loads(expected)['posts']) == 20

        conn.request('GET', '/feed', headers={'Accept-Encoding': 'gzip'})
        compressed = conn.getresponse()
        assert compressed.status == 200, compressed.status
        body = compressed.read()
        assert compressed.getheader('Content-Encoding') == 'gzip'
        assert compressed.getheader('Vary') == 'Accept-Encoding'
        assert int(compressed.getheader('Content-Length')) == len(body)
        assert gzip.decompress(body) == expected

    conn.request('GET', '/health', headers={'Accept-Encoding': 'gzip'})
    health = conn.getresponse()
    assert health.status == 200
    assert health.getheader('Content-Encoding') is None
    assert json.loads(health.read())['status'] == 'ok'
finally:
    conn.close()

print('PASS: repeated independent gzip members match plain feed responses; small responses stay plain')
