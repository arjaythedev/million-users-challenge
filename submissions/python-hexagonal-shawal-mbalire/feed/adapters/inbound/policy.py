"""Transport policy shared by every HTTP adapter, so swapping adapters cannot change wire behaviour.

Compression: only the feed (20 posts, ~4 KB) is worth it. A single post or an error is a few
hundred bytes, where the gzip header and the CPU cost outweigh the bytes saved. Level 1 keeps most
of level 9's ratio on JSON text at a fraction of its CPU, and CPU is what caps a Python server.
"""
import zlib
from typing import NamedTuple

GZIP_MIN_BYTES = 1024
GZIP_LEVEL = 1
KEEP_ALIVE_S = 120  # the contract asks for >= 75 s; a margin so a client at 75 s never races the close
LISTEN_BACKLOG = 65535  # = net.core.somaxconn on the box; absorbs the ~3,300 connections/s ramp


def gzip(body: bytes) -> bytes:
    c = zlib.compressobj(GZIP_LEVEL, zlib.DEFLATED, 31)  # wbits 31: the gzip container
    return c.compress(body) + c.flush()


def encode(body: bytes, accepts_gzip: bool) -> tuple[bytes, bool]:
    """-> (wire body, gzipped?)"""
    if accepts_gzip and len(body) >= GZIP_MIN_BYTES:
        return gzip(body), True
    return body, False


class Listen(NamedTuple):
    """Where a worker serves. `bind(port)` makes this worker's own SO_REUSEPORT socket; servers
    that bind for themselves (granian) use host and ports instead."""
    host: str
    ports: tuple[int, ...]
    bind: object  # Callable[[int], socket.socket]
