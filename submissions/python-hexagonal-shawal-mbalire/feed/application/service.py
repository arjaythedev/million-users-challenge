"""The use cases: the driving port. One method per endpoint; transport-shaped input in, `Reply` out.

Each method carries two declarations: `@route` (the API contract, adapted by every HTTP adapter)
and `@query` / `@command` (the domain semantics, adapted by the profiler and any policy that
cares whether an operation changes state).

Inputs stay raw (`str` path segment, `bytes` body, the header as received) so that every rule
about them lives here or in the domain, once, and in the order the spec requires: on an
authenticated route the token is checked *before* the id, so `POST /posts/abc/like` without a
token is a 401, not a 400. A framework that validated path parameters itself would get that order
wrong, which is exactly why adapters pass the raw segment through.

Unexpected exceptions are not caught here: an adapter turns them into a 500, the one failure the
core cannot describe.
"""
import time

from ..domain import rules
from ..domain.errors import DomainError, MalformedJson, PostNotFound
from ..domain.semantics import command, query
from .contract import Reply, reply_for
from .ports import Authenticator, JsonCodec, PostRepository
from .routes import route


class FeedService:
    __slots__ = ("_posts", "_auth", "_json", "_started")

    def __init__(self, posts: PostRepository, auth: Authenticator, codec: JsonCodec,
                 started: float | None = None):
        self._posts = posts
        self._auth = auth
        self._json = codec
        self._started = time.monotonic() if started is None else started

    @route("GET", "/health")
    @query
    async def health(self) -> Reply:
        try:
            await self._posts.ping()
        except Exception as exc:  # any failure to answer SELECT 1 is "unreachable", by definition
            return Reply(503, self._json.degraded(str(exc) or type(exc).__name__))
        return Reply(200, self._json.health(int(time.monotonic() - self._started)))

    @route("GET", "/feed")
    @query
    async def feed(self) -> Reply:
        return Reply(200, self._json.feed(await self._posts.newest(rules.FEED_SIZE)))

    @route("GET", "/posts/{id}", needs=("id",))
    @query
    async def post(self, raw_id: str) -> Reply:
        try:
            post = await self._posts.by_id(rules.parse_post_id(raw_id))
            if post is None:
                raise PostNotFound
        except DomainError as error:
            return reply_for(error)
        return Reply(200, self._json.post(post))

    @route("POST", "/posts", needs=("authorization", "body"))
    @command
    async def create_post(self, authorization: str | None, raw_body: bytes) -> Reply:
        try:
            author = self._auth.authenticate(authorization)
            try:
                document = self._json.parse(raw_body)
            except ValueError:  # every codec's decode error, and UnicodeDecodeError, subclass it
                raise MalformedJson from None
            body = rules.normalize_body(document.get("body") if isinstance(document, dict) else None)
            post_id, created_at = await self._posts.create(author.user_id, body)
        except DomainError as error:
            return reply_for(error)
        return Reply(201, self._json.post((post_id, body, created_at, author.username, 0)))

    @route("POST", "/posts/{id}/like", needs=("authorization", "id"))
    @command(idempotent=True)
    async def like_post(self, authorization: str | None, raw_id: str) -> Reply:
        try:
            actor = self._auth.authenticate(authorization)
            post_id = rules.parse_post_id(raw_id)
            inserted = await self._posts.like(actor.user_id, post_id)
        except DomainError as error:
            return reply_for(error)
        return Reply(201 if inserted else 200, self._json.like(post_id, already_liked=not inserted))
