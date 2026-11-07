#!/usr/bin/env python3
"""Check partial requests, pipelining, chunked bodies, gzip and idle reuse."""
import argparse
import base64
import hashlib
import hmac
import os
import gzip
import json
import socket
import time
from urllib.parse import urlsplit
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('base')
p.add_argument('--idle',type=float,default=0)
a=p.parse_args();base=urlsplit(a.base)
s=socket.create_connection((base.hostname,base.port),timeout=10);s.settimeout(30);r=s.makefile('rb')
def response():
 line=r.readline();assert line.startswith(b'HTTP/1.1 '),line
 status=int(line.split()[1]);headers={}
 while True:
  line=r.readline();assert line,line
  if line==b'\r\n':break
  k,v=line.split(b':',1);headers[k.lower()]=v.strip()
 body=r.read(int(headers.get(b'content-length',b'0')))
 if headers.get(b'content-encoding')==b'gzip':body=gzip.decompress(body)
 return status,headers,json.loads(body)
# Split a header across TCP writes.
s.sendall(b'GET /feed HTTP/1.1\r\nHost: localhost\r\nAccept-Enc')
time.sleep(.01);s.sendall(b'oding: gzip\r\n\r\n')
status,headers,body=response();assert status==200 and len(body['posts'])==20,(status,body)
assert headers.get(b'content-encoding')==b'gzip',headers
# Both responses must survive a single pipelined TCP write.
s.sendall(b'GET /posts/1 HTTP/1.1\r\nHost: localhost\r\n\r\nGET /posts/2 HTTP/1.1\r\nHost: localhost\r\n\r\n')
assert response()[2]['post']['id']==1
assert response()[2]['post']['id']==2
# Consume a chunked body even when authentication fails, then reuse the socket.
s.sendall(b'POST /posts HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n5\r\n{"bod\r\n7\r\ny":"x"}\r\n0\r\n\r\nGET /posts/1 HTTP/1.1\r\nHost: localhost\r\n\r\n')
assert response()[0]==401
assert response()[2]['post']['id']==1
# A decoded chunked body must reach the real authenticated create path.
def b64(data):
 return base64.urlsafe_b64encode(data).rstrip(b'=')
signed=b64(b'{"alg":"HS256","typ":"JWT"}')+b'.'+b64(json.dumps({'sub':'1','username':'golden_ember_1','exp':int(time.time())+3600}).encode())
token=signed+b'.'+b64(hmac.new(os.environ.get('JWT_SECRET','million-users-challenge').encode(),signed,hashlib.sha256).digest())
body=b'{"body":"chunked transport test"}'
s.sendall(b'POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer '+token+b'\r\nTransfer-Encoding: chunked\r\n\r\n')
for chunk in (body[:5],body[5:]):
 s.sendall(format(len(chunk),'x').encode()+b'\r\n'+chunk+b'\r\n')
 time.sleep(.01)
s.sendall(b'0\r\n\r\n')
status,headers,created=response();assert status==201 and created['post']['body']=='chunked transport test',(status,created)
# An Expect client waits for the interim response before sending its body.
s.sendall(b'POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer '+token+b'\r\nExpect: 100-continue\r\nContent-Length: '+str(len(body)).encode()+b'\r\n\r\n')
assert r.readline()==b'HTTP/1.1 100 Continue\r\n'
assert r.readline()==b'\r\n'
s.sendall(body)
status,headers,created=response();assert status==201,(status,created)
# Compressor reuse must produce independent, complete gzip members.
for _ in range(3):
 s.sendall(b'GET /feed HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: gzip\r\n\r\n')
 status,headers,body=response()
 assert status==200 and len(body['posts'])==20 and headers.get(b'content-encoding')==b'gzip'
if a.idle:
 time.sleep(a.idle)
 s.sendall(b'GET /posts/1 HTTP/1.1\r\nHost: localhost\r\n\r\n')
 status,headers,body=response();assert status==200,(status,body)
r.close();s.close()
print('PASS: partial headers, pipelined requests, chunked body, gzip, idle reuse')
