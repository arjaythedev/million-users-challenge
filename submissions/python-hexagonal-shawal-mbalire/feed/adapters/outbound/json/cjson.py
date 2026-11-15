"""JsonCodec adapter: a purpose-built C encoder (`_cjson.c`).

Where the hand-written codec makes ~6 Python-level operations per post (one f-string, two escaper
calls, the join), this makes one C call per *response*: the whole feed, 20 posts, is walked and
written into a single buffer without creating any intermediate Python string. Decoding stays on
the stdlib's C parser: request bodies are 1% of traffic and tiny, so there is nothing to win.
"""
import json

from . import _cjson
from .handwritten import HandwrittenJson


class CJson(HandwrittenJson):
    __slots__ = ()
    post = staticmethod(_cjson.post)
    feed = staticmethod(_cjson.feed)
    parse = staticmethod(json.loads)
