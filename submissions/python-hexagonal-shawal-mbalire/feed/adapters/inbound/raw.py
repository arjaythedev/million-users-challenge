"""Driving adapter (server + app): HTTP/1.1 over raw sockets, written against uvloop's transports.

No framework and no server library. What an ASGI stack does per request (a scope dict, receive /
send coroutines, a middleware chain, every header parsed into a list of pairs) is replaced by
`bytes` slicing: the request line is split once, and the three headers the contract reads
(Content-Length, Authorization, Accept-Encoding) are found with `bytes.find` on a once-lowercased
copy of the head, a handful of C-level scans with no per-header Python objects. Routing is the
application's `Router`, the same one every other adapter uses.

Connection model
----------------
One `Protocol` per connection, `__slots__` only, so an idle keep-alive connection costs a few
hundred bytes of Python plus the kernel socket: a million of them fit in RAM easily. Idle
connections are never timed out by the server (the contract asks for at least 75 s; never is at
least 75 s), which also means no per-connection timer, the dominant idle cost in most servers.

Requests on one connection are answered strictly in order (HTTP/1.1 pipelining): while one is
awaiting the database, further bytes are buffered, not parsed.

Driving the use case without a Task
-----------------------------------
A use case is a coroutine that awaits one repository future and returns a `Reply`. An asyncio
Task around it would buy a context copy, a wrapper coroutine and the Task's own bookkeeping, for
nothing: a connection runs one request at a time and nobody ever cancels or joins it. So the
protocol steps the coroutine itself, as a Task would: `send(None)` runs it to its next await, the
awaited future's done-callback steps it again, and `StopIteration` carries the reply. The use cases
stay ordinary `async def`s and know nothing of this. Measured in isolation on uvloop with the
eager task factory: 0.71 µs per request against 1.45 µs for a Task, about 1% of a request.
"""
import asyncio

from ...application.contract import INTERNAL_ERROR, NOT_FOUND, Reply
from ...application.routes import Router
from . import policy

MAX_HEAD_BYTES = 16384
MAX_BODY_BYTES = 65536

_STATUS = {
    200: b"200 OK", 201: b"201 Created", 400: b"400 Bad Request", 401: b"401 Unauthorized",
    404: b"404 Not Found", 500: b"500 Internal Server Error", 503: b"503 Service Unavailable",
}
_PLAIN = {s: b"HTTP/1.1 " + line + b"\r\nContent-Type: application/json\r\nContent-Length: " for s, line in _STATUS.items()}
_GZIP = {s: b"HTTP/1.1 " + line + b"\r\nContent-Type: application/json\r\nContent-Encoding: gzip\r\nVary: Accept-Encoding\r\nContent-Length: " for s, line in _STATUS.items()}
_CLOSE = b"\r\nConnection: close\r\n\r\n"
_END = b"\r\n\r\n"


def _header(head: bytes, lower: bytes, name: bytes) -> bytes | None:
    """`name` is lowercase and framed as b"\\nname:"; returns the value from the original bytes."""
    i = lower.find(name)
    if i < 0:
        return None
    start = i + len(name)
    end = head.find(b"\r\n", start)
    return head[start:end if end >= 0 else len(head)].strip()


class HttpProtocol(asyncio.Protocol):
    __slots__ = ("_match", "_transport", "_buffer", "_busy", "_work", "_gzip_ok", "_close")

    def __init__(self, match):
        self._match = match
        self._transport: asyncio.Transport | None = None
        self._buffer = b""
        self._busy = False
        self._work = None  # the use-case coroutine in flight, if any
        self._gzip_ok = False
        self._close = False

    def connection_made(self, transport) -> None:
        self._transport = transport

    def connection_lost(self, exc) -> None:
        self._transport = None

    def data_received(self, data: bytes) -> None:
        if self._buffer:
            data = self._buffer + data
            self._buffer = b""
        if self._busy:
            self._buffer = data
        else:
            self._parse(data)

    def _parse(self, data: bytes) -> None:
        while data:
            head_end = data.find(_END)
            if head_end < 0:
                if len(data) > MAX_HEAD_BYTES:
                    return self._abort()
                self._buffer = data
                return
            head = data[:head_end]
            lower = head.lower()
            length = _header(head, lower, b"\ncontent-length:")
            try:
                body_len = int(length) if length else 0
            except ValueError:
                return self._abort()
            if body_len < 0 or body_len > MAX_BODY_BYTES:
                return self._abort()
            body_start = head_end + 4
            body_end = body_start + body_len
            if len(data) < body_end:
                self._buffer = data
                return
            body = data[body_start:body_end]
            data = data[body_end:]

            line_end = head.find(b"\r\n")
            line = head if line_end < 0 else head[:line_end]
            sp1 = line.find(b" ")
            sp2 = line.find(b" ", sp1 + 1)
            if sp1 < 0 or sp2 < 0:
                return self._abort()
            path = line[sp1 + 1:sp2]
            q = path.find(b"?")
            if q >= 0:
                path = path[:q]
            gzip_ok = b"gzip" in (_header(head, lower, b"\naccept-encoding:") or b"")
            close = lower.find(b"\nconnection: close") >= 0

            found = self._match(line[:sp1].decode("latin-1"), path.decode("latin-1"))
            if found is None:
                self._write(NOT_FOUND, gzip_ok, close)
                if close:
                    return
                continue
            endpoint, raw_id = found
            authorization = None
            if endpoint.wants_auth:
                value = _header(head, lower, b"\nauthorization:")
                authorization = None if value is None else value.decode("latin-1")
            # The request needs I/O: park the rest of the buffer and resume once it is answered.
            self._busy = True
            self._buffer = data
            self._work = endpoint.invoke(raw_id, authorization, body)
            self._gzip_ok = gzip_ok
            self._close = close
            self._step()
            return

    def _step(self, _future=None) -> None:
        """Run the use case to its next await (or its end), as Task.__step would."""
        try:
            awaited = self._work.send(None)
        except StopIteration as done:
            return self._finish(done.value)
        except (Exception, asyncio.CancelledError):  # a cancelled future must not wedge the connection
            return self._finish(INTERNAL_ERROR)
        if awaited is None:  # a bare yield (asyncio.sleep(0)): go round the loop once
            asyncio.get_running_loop().call_soon(self._step)
        else:
            awaited._asyncio_future_blocking = False
            awaited.add_done_callback(self._step)

    def _finish(self, reply: Reply) -> None:
        close = self._close
        self._work = None
        self._write(reply, self._gzip_ok, close)
        self._busy = False
        if self._buffer and self._transport is not None and not close:
            data, self._buffer = self._buffer, b""
            self._parse(data)

    def _write(self, reply: Reply, gzip_ok: bool, close: bool) -> None:
        transport = self._transport
        if transport is None:
            return
        body, gzipped = policy.encode(reply.body, gzip_ok)
        prefix = (_GZIP if gzipped else _PLAIN)[reply.status]
        transport.write(b"".join((prefix, str(len(body)).encode(), _CLOSE if close else _END, body)))
        if close:
            transport.close()

    def _abort(self) -> None:
        if self._transport is not None:
            self._transport.close()


def build(router: Router) -> Router:
    """The raw server needs no app object: it dispatches on the router directly."""
    return router


async def serve(router: Router, listen: policy.Listen) -> None:
    """Accept on this worker's own SO_REUSEPORT socket for each port, forever."""
    loop = asyncio.get_running_loop()
    match = router.match
    servers = [await loop.create_server(lambda: HttpProtocol(match), sock=listen.bind(port),
                                        backlog=policy.LISTEN_BACKLOG)
               for port in listen.ports]
    await asyncio.gather(*(server.serve_forever() for server in servers))
