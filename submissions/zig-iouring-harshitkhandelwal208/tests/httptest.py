#!/usr/bin/env python3
"""Raw-socket HTTP/1.1 and gzip checks against a running server (default 127.0.0.1:3000, second port 3001).

Run it on a fresh copy of the seed: it writes a few posts. Usage: httptest.py [port] [extra_port]
"""
import base64, hashlib, hmac, gzip, json, socket, sys, time, zlib

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 3000
PORT2 = int(sys.argv[2]) if len(sys.argv) > 2 else PORT + 1
SECRET = b"million-users-challenge"
fails = 0


def b64(b):
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def token(uid=1, name="golden_ember_1", exp=3600):
    now = int(time.time())
    h = b64(b'{"alg":"HS256","typ":"JWT"}')
    p = b64(json.dumps({"sub": str(uid), "username": name, "iat": now, "exp": now + exp}).encode())
    return f"{h}.{p}." + b64(hmac.new(SECRET, f"{h}.{p}".encode(), hashlib.sha256).digest())


AUTH = f"Authorization: Bearer {token()}\r\n"


def conn(port=PORT):
    s = socket.create_connection((HOST, port))
    s.settimeout(5)
    return s


def read_resp(s, buf=b""):
    """One response -> (status, headers, body, rest); status None when the peer closed first."""
    while b"\r\n\r\n" not in buf:
        d = s.recv(65536)
        if not d:
            return None, None, None, buf
        buf += d
    head, rest = buf.split(b"\r\n\r\n", 1)
    lines = head.decode().split("\r\n")
    status = int(lines[0].split()[1])
    hdrs = {l.split(":", 1)[0].lower(): l.split(":", 1)[1].strip() for l in lines[1:]}
    n = int(hdrs.get("content-length", 0)) if status != 204 else 0
    while len(rest) < n:
        d = s.recv(65536)
        if not d:
            break
        rest += d
    return status, hdrs, rest[:n], rest[n:]


def check(name, ok, detail=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  " + str(detail)))
    if not ok:
        fails += 1


def once(raw, port=PORT):
    s = conn(port)
    s.sendall(raw if isinstance(raw, bytes) else raw.encode())
    r = read_resp(s)
    s.close()
    return r


def closed_by_server(s):
    try:
        return s.recv(1) == b""
    except (socket.timeout, ConnectionResetError):
        return False


# ---- framing ---------------------------------------------------------------------------------

s = conn()
s.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\nGET /nope HTTP/1.1\r\nHost: x\r\n\r\n")
st1, _, b1, rest = read_resp(s)
st2, _, b2, _ = read_resp(s, rest)
check("pipelined requests are answered in order", st1 == 200 and st2 == 404 and b'"status":"ok"' in b1, (st1, st2))
s.close()

s = conn()
req = b"GET /posts/1 HTTP/1.1\r\nHost: x\r\n\r\n"
for i in range(len(req)):
    s.sendall(req[i:i + 1])
    time.sleep(0.002)
st, _, body, _ = read_resp(s)
check("request sent a byte at a time", st == 200 and body.startswith(b'{"post":{"id":1,'), (st, body[:60]))
s.close()

s = conn()
body = b'{"body":"split body test"}'
s.sendall(f"POST /posts HTTP/1.1\r\nHost: x\r\n{AUTH}Content-Length: {len(body)}\r\n\r\n".encode())
time.sleep(0.1)
s.sendall(body[:10])
time.sleep(0.1)
s.sendall(body[10:])
st, _, b, _ = read_resp(s)
check("head and body in separate packets", st == 201 and b'"split body test"' in b, (st, b[:80]))
s.close()

s = conn()
chunks = b'{"body":"chunk' + b'ed body"}'
s.sendall(f"POST /posts HTTP/1.1\r\nHost: x\r\n{AUTH}Transfer-Encoding: chunked\r\n\r\n".encode()
          + f"{len(chunks[:14]):x}\r\n".encode() + chunks[:14] + b"\r\n" + f"{len(chunks[14:]):x}\r\n".encode() + chunks[14:] + b"\r\n0\r\n\r\n")
st, _, b, _ = read_resp(s)
check("chunked request body", st == 201 and b'"chunked body"' in b, (st, b[:80]))
s.close()

for i in range(3):
    s = conn()
    s.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
    read_resp(s)
    s.sendall(b"GET /feed HTTP/1.1\r\nHost: x\r\n\r\n")
    st, _, _, _ = read_resp(s)
    if i == 2:
        check("many requests on one connection", st == 200)
    s.close()

s = conn()
s.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
st, hdrs, _, _ = read_resp(s)
check("Connection: close is answered and then closed", st == 200 and hdrs.get("connection") == "close" and closed_by_server(s), (st, hdrs))
s.close()

s = conn()
s.sendall(b"GET /health HTTP/1.0\r\n\r\n")
st, _, _, _ = read_resp(s)
check("HTTP/1.0 closes after the response", st == 200 and closed_by_server(s))
s.close()

s = conn()
s.sendall(b"GET /feed HTTP/1.1\r\nHost: x\r\n\r\n")
s.shutdown(socket.SHUT_WR)
st, _, body, _ = read_resp(s)
check("half-close still gets its response", st == 200 and body.startswith(b'{"posts":['), st)
s.close()

st, _, b, _ = once("GET /feed?limit=5&x=%20 HTTP/1.1\r\nHost: x\r\n\r\n")
check("query string is ignored", st == 200 and b.startswith(b'{"posts":['), st)

# ---- malformed input -------------------------------------------------------------------------

st, _, _, _ = once("GARBAGE\r\n\r\n")
check("garbage request line -> 400", st == 400, st)
st, _, _, _ = once("GET /health HTTP/2.0\r\n\r\n")
check("unsupported version -> 505", st == 505, st)
st, _, _, _ = once("GET /health HTTP/1.1\r\n" + "X-Pad: " + "a" * 20000 + "\r\n\r\n")
check("oversized head -> 431", st == 431, st)
st, _, _, _ = once(f"POST /posts HTTP/1.1\r\n{AUTH}Content-Length: 5\r\nContent-Length: 6\r\n\r\n{{\"a\":1}}")
check("conflicting Content-Length -> 400", st == 400, st)
st, _, _, _ = once(f"POST /posts HTTP/1.1\r\n{AUTH}Content-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n")
check("Content-Length with chunked -> 400", st == 400, st)
st, _, _, _ = once(f"POST /posts HTTP/1.1\r\n{AUTH}Content-Length: 99999999999\r\n\r\n")
check("absurd Content-Length -> 413/400", st in (400, 413), st)

# ---- API edge cases --------------------------------------------------------------------------

def post(body, auth=AUTH, port=PORT):
    raw = body if isinstance(body, bytes) else body.encode()
    return once(f"POST /posts HTTP/1.1\r\nHost: x\r\n{auth}Content-Type: application/json\r\nContent-Length: {len(raw)}\r\n\r\n".encode() + raw, port)


st, _, b, _ = post(json.dumps({"body": " 　 trimmed ﻿\t"}, ensure_ascii=False))
check("JavaScript whitespace is trimmed", st == 201 and json.loads(b)["post"]["body"] == "trimmed", (st, b[:100]))
st, _, b, _ = post(json.dumps({"body": "\U0001F600" * 500}))
check("500 astral characters are accepted", st == 201 and json.loads(b)["post"]["body"] == "\U0001F600" * 500, (st, b[:60]))
st, _, b, _ = post(json.dumps({"body": "\U0001F600" * 501}))
check("501 astral characters are refused", st == 400 and b'at most 500' in b, (st, b[:60]))
st, _, b, _ = post(json.dumps({"body": "q\"uote \\ back\nline \u0001 <&>/"}))
check("escapes round-trip", st == 201 and json.loads(b)["post"]["body"] == "q\"uote \\ back\nline \u0001 <&>/", (st, b[:100]))
st, _, b, _ = post("[1,2]")
check("array body -> 400", st == 400, (st, b))
st, _, b, _ = post("")
check("empty body -> body is required", st == 400 and b'required' in b, (st, b))
st, _, b, _ = post('{"body":"a"} trailing')
check("trailing garbage -> malformed", st == 400 and b'malformed' in b, (st, b))
st, _, b, _ = post(b'{"body":"bad utf8 \xff"}')
check("invalid UTF-8 -> malformed", st == 400 and b'malformed' in b, (st, b))
st, _, b, _ = post('{"body":"nul \\u0000 inside"}')
check("NUL character is a clean 500, not a crash", st == 500, (st, b))
st, _, b, _ = once("GET /health HTTP/1.1\r\n\r\n")
check("server still healthy after it", st == 200, st)
weird = 'we"ird \\ nam\u00e9 \u2713 \U0001F600'
st, _, b, _ = post('{"body":"user name round trip"}', "Authorization: Bearer " + token(7, weird) + "\r\n")
check("username with quotes/backslash/unicode is echoed escaped", st == 201 and json.loads(b)["post"]["author"] == weird, (st, b[:160]))
st, _, b, _ = post('{"body":"x"}', f"Authorization: Bearer {token(1, 'a', -10)}\r\n")
check("expired token -> 401", st == 401 and b'invalid or expired token' in b, (st, b))
st, _, b, _ = post('{"body":"x"}', "Authorization: bearer abc\r\n")
check("lower-case scheme is not Bearer -> missing", st == 401 and b'missing bearer token' in b, (st, b))
st, _, b, _ = post('{"body":"x"}', "Authorization:\r\n")
check("empty Authorization -> missing", st == 401 and b'missing bearer token' in b, (st, b))
for path, want in [("/posts/1/", 404), ("/posts//like", 404), ("/feed/", 404), ("/", 404), ("/posts/1/like/x", 404)]:
    st, _, _, _ = once(f"GET {path} HTTP/1.1\r\nHost: x\r\n\r\n")
    check(f"GET {path} -> {want}", st == want, st)
for pid, want in [("007", 200), ("+5", 400), ("1e3", 400), ("99999999999999999999", 404), ("0x10", 400), (" 1", 400)]:
    st, _, _, _ = once(f"GET /posts/{pid.replace(' ', '%20')} HTTP/1.1\r\nHost: x\r\n\r\n")
    check(f"GET /posts/{pid} -> {want}", st == want, st)

# paths where "/posts/" and "/like" overlap or nearly do (this used to build an invalid slice), and odd shapes
for method, path in [("POST", "/posts/like"), ("POST", "/posts/like/"), ("POST", "/posts/"), ("POST", "/posts//like"), ("POST", "/posts/1/lik"), ("POST", "/posts/1/likes"), ("POST", "/post/1/like"), ("POST", "/posts/1/like/x"), ("POST", "/posts/1//like")]:
    st, _, b, _ = once(f"{method} {path} HTTP/1.1\r\nHost: x\r\n{AUTH}Content-Length: 0\r\n\r\n")
    check(f"{method} {path} -> 404 (or 400 for a bad id)", st in (404, 400) and b.startswith(b'{"error"'), (st, b))
st, _, _, _ = once("GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
check("server alive after the odd paths", st == 200, st)

# chunked framing that would make the parser rescan a lot: refused quickly, and the server stays responsive
t0 = time.time()
s = conn()
s.sendall(b"POST /posts HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nx\r\n0\r\n")
try:
    for _ in range(3000):
        s.sendall(b"a:b\r\n" * 40)
except OSError:
    pass
st, _, _, _ = read_resp(s)
check("endless trailer lines are refused", st == 400, st)
s.close()
s = conn()
s.sendall(b"POST /posts HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5;" + b"e=" * 200 + b"\r\nhello\r\n0\r\n\r\n")
st, _, _, _ = read_resp(s)
check("a size line with a huge extension is refused", st == 400, st)
s.close()
s = conn()
s.sendall(b"POST /posts HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n" + b"1\r\nx\r\n" * 1100)
st, _, _, _ = read_resp(s)
check("more than 1024 chunks is refused", st in (400, 413), st)
s.close()
st, _, _, _ = once("GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
check(f"server responsive after chunk abuse ({time.time() - t0:.1f}s)", st == 200 and time.time() - t0 < 10, st)

# a large pipelined burst on one connection (consuming each request used to move the whole remaining buffer)
N = 60000
burst = b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n" * N
t0 = time.time()
s = conn()
s.settimeout(60)
sender = __import__("threading").Thread(target=lambda: s.sendall(burst))
sender.start()
got = 0
buf = b""
while got < N:
    d = s.recv(1 << 20)
    if not d:
        break
    buf += d
    got = buf.count(b"HTTP/1.1 200 OK")
sender.join()
el = time.time() - t0
check(f"{N} pipelined requests on one connection all answered in {el:.1f}s", got == N and el < 30, (got, el))
s.close()

# ---- gzip ------------------------------------------------------------------------------------

def feed(extra="", port=PORT):
    return once(f"GET /feed HTTP/1.1\r\nHost: x\r\n{extra}\r\n", port)


st, h0, plain, _ = feed()
check("no Accept-Encoding -> plain", st == 200 and "content-encoding" not in h0 and plain.startswith(b'{"posts":['), (st, h0))
for ae in ["gzip", "gzip, deflate, br", "deflate, GZIP", "br;q=1.0, gzip;q=0.8"]:
    st, h, gz, _ = feed(f"Accept-Encoding: {ae}\r\n")
    ok = st == 200 and h.get("content-encoding") == "gzip" and len(gz) < len(plain)
    if ok:
        try:
            ok = gzip.decompress(gz) == plain and zlib.decompressobj(31).decompress(gz) == plain
        except Exception as e:
            ok = False
            print("   ", e)
    check(f"Accept-Encoding: {ae} -> valid gzip equal to the plain body", ok, (st, h))
st, h, b, _ = feed("Accept-Encoding: gzip;q=0\r\n")
check("gzip;q=0 -> plain", st == 200 and "content-encoding" not in h, (st, h))
st, h, b, _ = feed("Accept-Encoding: identity\r\n")
check("identity -> plain", st == 200 and "content-encoding" not in h, (st, h))
st, h, b, _ = once("GET /posts/1 HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\n\r\n")
check("a small single post is not compressed", st == 200 and "content-encoding" not in h, (st, h))
st, h, b, _ = once("GET /nope HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\n\r\n")
check("errors are not compressed", st == 404 and "content-encoding" not in h and b == b'{"error":"not found"}', (st, h))
st, h, gz, _ = feed("Accept-Encoding: gzip\r\n", PORT2)
check("second port serves the same API (gzip)", st == 200 and h.get("content-encoding") == "gzip" and gzip.decompress(gz).startswith(b'{"posts":['), (st, h))

# a feed that has grown large (posts full of characters that need escaping) still compresses correctly
heavy = "\u0001\"\\\n\t" * 100
for _ in range(3):
    post(json.dumps({"body": heavy}))
st, h, gz, _ = feed("Accept-Encoding: gzip\r\n")
_, _, plain2, _ = feed()
check("feed with escape-heavy posts: gzip == plain", st == 200 and gzip.decompress(gz) == plain2 and len(plain2) > 3000, (st, len(plain2)))

print(f"\n{fails} failures")
sys.exit(1 if fails else 0)
