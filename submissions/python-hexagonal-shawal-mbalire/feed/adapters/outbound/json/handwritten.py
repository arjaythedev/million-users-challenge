"""JsonCodec adapter: hand-written f-strings over the stdlib's C string escaper. No dependencies.

The spec fixes key order and compact form byte for byte, and the values are only ints and
strings. A generic serializer must rediscover that structure for every object on every call;
here it is written once, and the only per-value work left is escaping, done by
`json.encoder.encode_basestring` (the C function json.dumps itself uses). It escapes `"`, `\\` and
control characters and leaves non-ASCII as UTF-8, which the spec allows.

`created_at` is not escaped: it comes from `feed.domain.timefmt`, digits and `-:.TZ` only.
"""
import json
from json.encoder import encode_basestring as _q

from ....domain.model import PostView


def _post(p: PostView) -> str:
    return f'{{"id":{p[0]},"body":{_q(p[1])},"created_at":"{p[2]}","author":{_q(p[3])},"like_count":{p[4]}}}'


class HandwrittenJson:
    __slots__ = ()

    def post(self, view: PostView) -> bytes:
        return f'{{"post":{_post(view)}}}'.encode()

    def feed(self, views: list[PostView]) -> bytes:
        return f'{{"posts":[{",".join(map(_post, views))}]}}'.encode()

    def like(self, post_id: int, already_liked: bool) -> bytes:
        return f'{{"liked":true,"already_liked":{"true" if already_liked else "false"},"post_id":{post_id}}}'.encode()

    def health(self, uptime_s: int) -> bytes:
        return f'{{"status":"ok","db":"ok","uptime_s":{uptime_s}}}'.encode()

    def degraded(self, error: str) -> bytes:
        return f'{{"status":"degraded","db":"unreachable","error":{_q(error)}}}'.encode()

    parse = staticmethod(json.loads)
