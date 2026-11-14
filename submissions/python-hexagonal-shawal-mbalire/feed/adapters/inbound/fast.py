"""Driving adapter: the worker's event loop in C (`_pq.Loop`, see _fastloop.c), for the score.

The two hot reads are served entirely in C, without entering Python. Everything else arrives here
over the bridge, and runs the same `Router` and use cases as every other adapter: writes, JWTs,
/health, every error, and anything the C parser does not recognise with certainty.

`FEED_NATIVE_READS=0` sends every request over the bridge, which measures what the loop alone buys.
"""
import asyncio
import socket

from ...application.contract import ERRORS, INTERNAL_ERROR, NOT_FOUND, Reply
from ...application.routes import Router
from ...domain import capacity
from ...domain.errors import PostNotFound
from ..outbound.postgres._pq import Loop
from ..outbound.postgres.libpq import STATEMENTS, opener, reconnect_in_background
from . import policy
from .raw import _CLOSE, _END, _GZIP, _PLAIN

_TCP_THIN_LINEAR_TIMEOUTS = 16  # <netinet/tcp.h>; not in Python's socket module


class Waiter:
    """What a use case awaits: the part of a Future that lanes and this driver use. There is no
    Task and no scheduling: the callback runs when the result is set, on the event that delivered it."""
    __slots__ = ("_done", "_result", "_exception", "_callback")

    def __init__(self):
        self._done = False
        self._result = self._exception = self._callback = None

    def done(self) -> bool:
        return self._done

    def set_result(self, result) -> None:
        self._result = result
        self._finish()

    def set_exception(self, exception) -> None:
        self._exception = exception
        self._finish()

    def _finish(self) -> None:
        self._done = True
        if self._callback is not None:
            self._callback()

    def add_done_callback(self, callback) -> None:
        self._callback = callback

    def __await__(self):
        if not self._done:
            yield self
        if self._exception is not None:
            raise self._exception
        return self._result


class FastLoop(Loop):
    """The loop, plus what is natural to write in Python: setting up and reconnecting."""

    def __init__(self, cfg):
        super().__init__()
        self._cfg = cfg
        self._open = opener(cfg.database_url, STATEMENTS)
        self._compressor = None
        # One read lane stays with the Python path, for /health. The rest serve reads natively.
        self.native_lanes = max(cfg.read_lanes - 1, 1) if cfg.native_reads else 0
        self.python_read_lanes = 1 if cfg.native_reads else cfg.read_lanes

    def create_future(self) -> Waiter:
        return Waiter()

    def use_compressor(self, capsule) -> None:
        self._compressor = capsule

    def run_forever(self, setup) -> None:
        """Runs the worker's setup coroutine, which never waits on anything but a real lane's
        connect (blocking) until `serve` has handed over, and then the loop itself."""
        asyncio._set_running_loop(self)
        try:
            setup.send(None)
        except StopIteration:
            pass
        self.run()

    def lane_down(self, index: int) -> None:
        reconnect_in_background(self, self._open, lambda conn: self.attach_lane(index, conn))


class _Request:
    """One bridged request, stepped through its use case as raw.py's protocol does."""
    __slots__ = ("_loop", "_fd", "_gen", "_work", "_gzip_ok", "_close")

    def __init__(self, loop, fd, gen, work, gzip_ok, close):
        self._loop = loop
        self._fd = fd
        self._gen = gen
        self._work = work
        self._gzip_ok = gzip_ok
        self._close = close

    def step(self) -> None:
        try:
            awaited = self._work.send(None)
        except StopIteration as done:
            return respond(self._loop, self._fd, self._gen, done.value, self._gzip_ok, self._close)
        except Exception:
            return respond(self._loop, self._fd, self._gen, INTERNAL_ERROR, self._gzip_ok, self._close)
        awaited.add_done_callback(self.step)


def respond(loop, fd: int, gen: int, reply: Reply, gzip_ok: bool, close: bool) -> None:
    body, gzipped = policy.encode(reply.body, gzip_ok)
    prefix = (_GZIP if gzipped else _PLAIN)[reply.status]
    loop.respond(fd, gen, b"".join((prefix, str(len(body)).encode(), _CLOSE if close else _END, body)), close)


def bridge_for(loop, router: Router):
    match = router.match

    def bridge(fd, gen, method, path, authorization, body, gzip_ok, close):
        found = match(method, path)
        if found is None:
            return respond(loop, fd, gen, NOT_FOUND, gzip_ok, close)
        endpoint, raw_id = found
        _Request(loop, fd, gen, endpoint.invoke(raw_id, authorization if endpoint.wants_auth else None, body),
                 gzip_ok, close).step()
    return bridge


def tune_listener(sock: socket.socket) -> None:
    """Options accepted connections inherit, so a million of them cost no setsockopt each."""
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    # Accept once the first request has arrived: one wake-up per new connection, not two.
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_DEFER_ACCEPT, 1)
    # Let a response carry the ACK of its request, from a connection's first request on.
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_QUICKACK, 0)
    # A thin stream's lost packet waits one RTO each time, not 200, 400, 800 ms.
    sock.setsockopt(socket.IPPROTO_TCP, _TCP_THIN_LINEAR_TIMEOUTS, 1)


async def serve(router: Router, listen: policy.Listen) -> None:
    loop = asyncio.get_running_loop()
    for _ in range(loop.native_lanes):
        loop.add_lane(loop._open())
    loop.serve(
        bridge=bridge_for(loop, router),
        on_lane_down=loop.lane_down,
        compressor=loop._compressor,
        gzip_min=policy.GZIP_MIN_BYTES,
        not_found=ERRORS[PostNotFound].body,
        internal_error=INTERNAL_ERROR.body,
        window_us=loop._cfg.batch_window_us,
        light_pass=capacity.LIGHT_PASS_EVENTS,
    )
    sockets = [listen.bind(port) for port in listen.ports]
    for sock in sockets:
        tune_listener(sock)
        loop.listen(sock.fileno())
    loop._sockets = sockets  # the loop owns their descriptors from here on
