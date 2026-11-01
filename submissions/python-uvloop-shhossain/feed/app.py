"""HTTP/1.1 and the five endpoints of SPEC.md, run by every worker process on its own event loop.

An idle keep-alive connection is one small Http object (seven slots) plus uvloop's transport: no buffer, no
timer. Reads land in uvloop's shared per-loop buffer and arrive here as one bytes object; a request is parsed
straight out of it. Requests on one connection are answered in order: while one waits for Postgres, anything
else that arrives on that connection is held in `buf` until the response has been written.

Queries don't run in asyncio Tasks: route() returns the statement to run and a callback, and the pool drives
asyncpg's coroutine directly (db._step), so a request creates no Task, no handler coroutine and no Future of its own.
"""
import asyncio
import time

from asyncpg.exceptions import ForeignKeyViolationError
from isal.isal_zlib import compress
from orjson import dumps, loads

from . import auth, db

MAX_HEAD = 64 * 1024
MAX_BODY = 64 * 1024
MAX_ID = auth.MAX_ID
BODY_MAX_CHARS = 500
# ECMAScript WhiteSpace + LineTerminator: what JavaScript's String.prototype.trim removes.
JS_WHITESPACE = ("\t\n\x0b\x0c\r \xa0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a"
                 "\u2028\u2029\u202f\u205f\u3000\ufeff")

# Status line + Content-Type, the start of every response.
STATUS = {code: b"HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n" % (code, reason) for code, reason in (
    (200, b"OK"), (201, b"Created"), (400, b"Bad Request"), (401, b"Unauthorized"), (404, b"Not Found"),
    (413, b"Payload Too Large"), (431, b"Request Header Fields Too Large"), (500, b"Internal Server Error"),
    (503, b"Service Unavailable"))}
OK = STATUS[200]
CREATED = STATUS[201]
# `close`: 0 keep-alive (HTTP/1.1), 1 close after this response, 2 HTTP/1.0 client that asked for keep-alive.
CONNECTION = (b"", b"Connection: close\r\n", b"Connection: keep-alive\r\n")
CONTINUE = b"HTTP/1.1 100 Continue\r\n\r\n"

NOT_FOUND = "not found"
INVALID_ID = "invalid post id"
POST_NOT_FOUND = "post not found"

# Set by start() in each worker.
POOL = None
LOG = print
GZIP_LEVEL = 0
GZIP_MIN = 1024
STARTED = time.time()


def render(status, body, gz, close):
    if gz and len(body) >= GZIP_MIN:
        body = compress(body, GZIP_LEVEL, 31)
        return b"%sContent-Encoding: gzip\r\nVary: Accept-Encoding\r\n%sContent-Length: %d\r\n\r\n%s" % (
            status, CONNECTION[close], len(body), body)
    return b"%s%sContent-Length: %d\r\n\r\n%s" % (status, CONNECTION[close], len(body), body)


def error(code, message, close):
    return render(STATUS[code], b'{"error":"%s"}' % message.encode(), False, close)


def server_error(e, close):
    LOG(f"internal error: {e!r}")
    return error(500, "internal server error", close)


def header(head, low, name):
    """Value of header `name` (b'\\r\\nlowercase-name:'), from the original-case header block, or None."""
    i = low.find(name)
    if i < 0:
        return None
    i += len(name)
    j = head.find(b"\r\n", i)
    return (head[i:] if j < 0 else head[i:j]).strip()


def accepts_gzip(v):
    if v == b"gzip":
        return True
    # "gzip, deflate, br", "deflate;q=0.5, gzip"; "gzip;q=0" is a refusal.
    for item in v.split(b","):
        coding, _, params = item.partition(b";")
        if coding.strip() in (b"gzip", b"*"):
            q = params.strip()
            return not (q[:2] == b"q=" and q[2:].strip(b"0.") == b"")
    return False


def post_id(s):
    """A positive integer in ASCII digits ('0', '-1', '1.5', 'abc' and '' are not), or None."""
    if s.isdigit():
        n = int(s)
        if n > 0:
            return n
    return None


def new_post_body(raw):
    """The trimmed `body` of POST /posts, or (None, error message)."""
    try:
        obj = loads(raw)
    except ValueError:
        return None, "malformed JSON body"
    text = obj.get("body") if obj.__class__ is dict else None
    if text.__class__ is not str:
        return None, "body is required"
    text = text.strip(JS_WHITESPACE)
    if not text:
        return None, "body is required"
    # Characters = code points, like Postgres char_length() in the CHECK constraint.
    if len(text) > BODY_MAX_CHARS:
        return None, "body must be at most 500 characters"
    return text, None


class Http(asyncio.Protocol):
    # t, buf, busy, cont: the connection. gz, close, arg: the request waiting for Postgres (one at a time).
    __slots__ = ("t", "buf", "busy", "cont", "gz", "close", "arg")

    def connection_made(self, transport):
        self.t = transport
        self.buf = None
        self.busy = False
        self.cont = False  # sent "100 Continue" for the request in buf

    def connection_lost(self, exc):
        self.t = None
        self.buf = None

    def data_received(self, data):
        if self.buf is not None:
            data = self.buf + data
            self.buf = None
        if self.busy:
            self.buf = data
        else:
            self.handle(data)

    def abort(self, code):
        """Unparseable or oversized request: answer and close."""
        t = self.t
        self.busy = True
        self.buf = None
        t.write(error(code, "bad request" if code == 400 else "request too large", 1))
        t.close()

    def handle(self, data):
        while True:
            end = data.find(b"\r\n\r\n")
            if end < 0:
                if len(data) > MAX_HEAD:
                    return self.abort(431)
                self.buf = data
                return
            le = data.find(b"\r\n")
            sp1 = data.find(b" ", 0, le)
            sp2 = data.find(b" ", sp1 + 1, le)
            if sp1 <= 0 or sp2 < 0:
                return self.abort(400)
            head = data[le:end]  # b"\r\nName: value\r\nName: value", no final CRLF
            low = head.lower()

            n = 0
            v = header(head, low, b"\r\ncontent-length:")
            if v is not None:
                if not v.isdigit():
                    return self.abort(400)
                n = int(v)
                if n > MAX_BODY:
                    return self.abort(413)
            if b"\r\ntransfer-encoding:" in low:
                return self.abort(400)  # chunked request bodies are not supported
            body_at = end + 4
            req_end = body_at + n
            if len(data) < req_end:
                if not self.cont:
                    v = header(head, low, b"\r\nexpect:")
                    if v is not None and v.lower() == b"100-continue":
                        self.cont = True
                        self.t.write(CONTINUE)
                self.buf = data
                return
            self.cont = False

            close = 0
            v = header(low, low, b"\r\nconnection:")
            version = data[sp2 + 1:le]
            if version != b"HTTP/1.1":
                close = 2 if v is not None and b"keep-alive" in v else 1
            if v is not None and b"close" in v:
                close = 1
            v = header(low, low, b"\r\naccept-encoding:")
            gz = v is not None and accepts_gzip(v)

            method = data[:sp1]
            target = data[sp1 + 1:sp2]
            q = target.find(b"?")
            path = target if q < 0 else target[:q]
            r = self.route(method, path, head, low, data[body_at:req_end], gz, close)
            rest = data[req_end:]
            if r.__class__ is bytes:
                self.t.write(r)
                if close == 1:
                    self.busy = True
                    self.t.close()
                    return
                if not rest:
                    return
                data = rest
                continue
            # A query: hold whatever came after this request until the response is written.
            self.busy = True
            self.gz = gz
            self.close = close
            if rest:
                self.buf = rest
            POOL.query(*r)
            return

    def route(self, method, path, head, low, body, gz, close):
        """Either the complete response (bytes) or the query to run: (statement, args, callback)."""
        if method == b"GET":
            if path == b"/feed":
                return db.FEED, (), self.feed
            if path[:7] == b"/posts/":
                rest = path[7:]
                if b"/" in rest:
                    return error(404, NOT_FOUND, close)
                pid = post_id(rest)
                if pid is None:
                    return error(400, INVALID_ID, close)
                if pid > MAX_ID:
                    return error(404, POST_NOT_FOUND, close)
                return db.POST, (pid,), self.post
            if path == b"/health":
                return db.HEALTH, (), self.health, 5.0
        elif method == b"POST":
            if path == b"/posts":
                user = auth.verify(header(head, low, b"\r\nauthorization:"), time.time())
                if user.__class__ is str:
                    return error(401, user, close)
                text, err = new_post_body(body)
                if err is not None:
                    return error(400, err, close)
                self.arg = (text, user[1])
                return db.INSERT_POST, (user[0], text), self.create
            if path[:7] == b"/posts/":
                rest = path[7:]
                i = rest.find(b"/")
                if i >= 0 and rest[i:] == b"/like":
                    # Auth first, then the id: POST /posts/abc/like without a token is a 401.
                    user = auth.verify(header(head, low, b"\r\nauthorization:"), time.time())
                    if user.__class__ is str:
                        return error(401, user, close)
                    pid = post_id(rest[:i])
                    if pid is None:
                        return error(400, INVALID_ID, close)
                    if pid > MAX_ID:
                        return error(404, POST_NOT_FOUND, close)
                    self.arg = pid
                    return db.INSERT_LIKE, (user[0], pid), self.like
        return error(404, NOT_FOUND, close)

    def done(self, out):
        t = self.t
        if t is None:
            return  # the client went away while we waited for Postgres
        t.write(out)
        if self.close == 1:
            t.close()
            return
        self.busy = False
        data = self.buf
        if data is not None:
            self.buf = None
            self.handle(data)

    # Query callbacks: (rows, command tag, exception), exception is None on success. Whatever goes wrong in one,
    # Postgres or rendering, ends as a 500 on this connection rather than leaving it waiting.

    def feed(self, rows, _, exc):
        try:
            if exc is not None:
                raise exc
            out = render(OK, dumps({"posts": [
                {"id": i, "body": b, "created_at": t, "author": a, "like_count": n} for i, b, t, a, n in rows
            ]}), self.gz, self.close)
        except Exception as e:
            out = server_error(e, self.close)
        self.done(out)

    def post(self, rows, _, exc):
        try:
            if exc is not None:
                raise exc
            if rows:
                i, b, t, a, n = rows[0]
                out = render(OK, dumps({"post": {"id": i, "body": b, "created_at": t, "author": a, "like_count": n}}),
                             self.gz, self.close)
            else:
                out = error(404, POST_NOT_FOUND, self.close)
        except Exception as e:
            out = server_error(e, self.close)
        self.done(out)

    def create(self, rows, _, exc):
        text, name = self.arg
        self.arg = None
        try:
            if exc is not None:
                raise exc
            pid, created = rows[0]
            out = render(CREATED, dumps({"post": {"id": pid, "body": text, "created_at": created, "author": name,
                                                  "like_count": 0}}), self.gz, self.close)
        except Exception as e:
            out = server_error(e, self.close)
        self.done(out)

    def like(self, _, status, exc):
        pid = self.arg
        self.arg = None
        try:
            if exc is not None:
                raise exc
            # Command tag: INSERT 0 1 = a new like, INSERT 0 0 = ON CONFLICT DO NOTHING (already liked).
            if status == b"INSERT 0 1":
                out = render(CREATED, b'{"liked":true,"already_liked":false,"post_id":%d}' % pid, self.gz, self.close)
            else:
                out = render(OK, b'{"liked":true,"already_liked":true,"post_id":%d}' % pid, self.gz, self.close)
        except ForeignKeyViolationError:  # SQLSTATE 23503: no such post
            out = error(404, POST_NOT_FOUND, self.close)
        except Exception as e:
            out = server_error(e, self.close)
        self.done(out)

    def health(self, _, __, exc):
        if exc is None:
            out = render(OK, b'{"status":"ok","db":"ok","uptime_s":%d}' % int(time.time() - STARTED), self.gz,
                         self.close)
        else:
            out = render(STATUS[503], dumps({"status": "degraded", "db": "unreachable",
                                             "error": str(exc) or exc.__class__.__name__}), self.gz, self.close)
        self.done(out)


async def start(cfg, worker):
    """Connects this worker's Postgres pool, then opens its listeners."""
    global POOL, LOG, GZIP_LEVEL, GZIP_MIN, STARTED
    loop = asyncio.get_running_loop()
    LOG = db.log_to_stderr(f"worker {worker}")
    GZIP_LEVEL, GZIP_MIN, STARTED = cfg.gzip_level, cfg.gzip_min, cfg.started
    POOL = db.Pool(cfg.database_url, cfg.db_per_worker, LOG)
    # Listen only once the database is connected, so the kernel never hands this worker a connection it can't
    # serve yet. If Postgres is unreachable, listen anyway after a while so /health can say so.
    await POOL.start(wait_s=20.0)
    servers = []
    for sock in cfg.make_sockets():
        servers.append(await loop.create_server(Http, sock=sock, backlog=65535))
    return servers
