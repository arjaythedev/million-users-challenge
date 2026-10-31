"""JsonCodec adapter: Pydantic v2 models, the FastAPI-idiomatic way.

Every value is *validated* into a model (type checks per field), then serialized by
pydantic-core (Rust). Field order is the wire key order. Validation buys nothing here, since the
values come from typed database columns, so this adapter measures the price of the declarative
style when it is applied to output that is already known to be correct.
"""
import pydantic_core
from pydantic import BaseModel

from ....domain.model import PostView


class Post(BaseModel):
    id: int
    body: str
    created_at: str
    author: str
    like_count: int


class PostEnvelope(BaseModel):
    post: Post


class Feed(BaseModel):
    posts: list[Post]


class Like(BaseModel):
    liked: bool
    already_liked: bool
    post_id: int


class Health(BaseModel):
    status: str
    db: str
    uptime_s: int


class Degraded(BaseModel):
    status: str
    db: str
    error: str


def _post(p: PostView) -> Post:
    return Post(id=p[0], body=p[1], created_at=p[2], author=p[3], like_count=p[4])


class PydanticJson:
    __slots__ = ()

    def post(self, view: PostView) -> bytes:
        model = PostEnvelope(post=_post(view))
        return model.__pydantic_serializer__.to_json(model)

    def feed(self, views: list[PostView]) -> bytes:
        model = Feed(posts=[_post(v) for v in views])
        return model.__pydantic_serializer__.to_json(model)

    def like(self, post_id: int, already_liked: bool) -> bytes:
        model = Like(liked=True, already_liked=already_liked, post_id=post_id)
        return model.__pydantic_serializer__.to_json(model)

    def health(self, uptime_s: int) -> bytes:
        model = Health(status="ok", db="ok", uptime_s=uptime_s)
        return model.__pydantic_serializer__.to_json(model)

    def degraded(self, error: str) -> bytes:
        model = Degraded(status="degraded", db="unreachable", error=error)
        return model.__pydantic_serializer__.to_json(model)

    parse = staticmethod(pydantic_core.from_json)
