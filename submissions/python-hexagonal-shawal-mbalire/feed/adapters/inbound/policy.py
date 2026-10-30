"""Transport policy shared by every HTTP adapter, so swapping adapters cannot change wire behaviour.

Compression: only the feed (20 posts, ~4 KB) is worth it. A single post or an error is a few
hundred bytes, where the gzip header and the CPU cost outweigh the bytes saved. Level 1 keeps most
of level 9's ratio on JSON text at a fraction of its CPU, and CPU is what caps a Python server.

The compressor is sized for the body, not for zlib's defaults. A default deflate stream allocates
~256 KB of state (a 32 KB window plus 64 KB hash tables) per call. glibc then hands it back to the
kernel on free, so every feed response paid a heap grow (brk), fresh page faults, and a 64 KB
memset. Measured in the server: gzip cost ~55 µs per feed response, a third of the worker's CPU.
A 4 KB window (the feed is ~3.6 KB; JSON repeats its keys every ~180 bytes) and memLevel 4 need
~24 KB, which stays inside the malloc arena: same output size on the real feed, a fraction of the
cost.

Which compressor runs is the composition root's choice (`FEED_GZIP`, through `use_gzip`):
`zlib_gzip` below, or `_deflate.gzip` (libdeflate, one compressor per worker, no per-call
state at all). Both emit standard gzip; tests/test_policy.py round-trips each.
"""
import zlib
from typing import NamedTuple

GZIP_MIN_BYTES = 1024
GZIP_LEVEL = 1
GZIP_WBITS = 16 + 12  # gzip container, 4 KB window
GZIP_MEM_LEVEL = 4
KEEP_ALIVE_S = 120  # the contract asks for >= 75 s; a margin so a client at 75 s never races the close
LISTEN_BACKLOG = 65535  # = net.core.somaxconn on the box; absorbs the ~3,300 connections/s ramp


def zlib_gzip(body: bytes) -> bytes:
    c = zlib.compressobj(GZIP_LEVEL, zlib.DEFLATED, GZIP_WBITS, GZIP_MEM_LEVEL)
    return c.compress(body) + c.flush()


gzip = zlib_gzip


def use_gzip(compress) -> None:
    """Install the worker's compressor (Callable[[bytes], bytes]) for every adapter using `encode`."""
    global gzip
    gzip = compress


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
