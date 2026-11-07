"""JsonCodec adapter: orjson (Rust), the fastest general-purpose Python JSON library.

General-purpose means it serializes *dicts*: each post is first built as a dict (5 key inserts),
then walked again by the encoder. The comparison with `cjson`, which reads the tuples directly,
measures exactly the cost of that intermediate representation.
"""
import orjson

from ....domain.model import PostView


def _post(p: PostView) -> dict:
    return {"id": p[0], "body": p[1], "created_at": p[2], "author": p[3], "like_count": p[4]}


class OrJson:
    __slots__ = ()

    def post(self, view: PostView) -> bytes:
        return orjson.dumps({"post": _post(view)})

    def feed(self, views: list[PostView]) -> bytes:
        return orjson.dumps({"posts": [_post(v) for v in views]})

    def like(self, post_id: int, already_liked: bool) -> bytes:
        return orjson.dumps({"liked": True, "already_liked": already_liked, "post_id": post_id})

    def health(self, uptime_s: int) -> bytes:
        return orjson.dumps({"status": "ok", "db": "ok", "uptime_s": uptime_s})

    def degraded(self, error: str) -> bytes:
        return orjson.dumps({"status": "degraded", "db": "unreachable", "error": error})

    parse = staticmethod(orjson.loads)
