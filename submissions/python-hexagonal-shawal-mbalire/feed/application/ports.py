"""Driven ports: what the use cases need from the outside world, and nothing about how.

These are `typing.Protocol`s, i.e. structural types. An adapter satisfies a port by having the
right methods; it never imports or subclasses anything from the core. That keeps the dependency
arrow pointing inward, and it costs nothing at run time: a call through a port is the same
bound-method call it would be without one (there is no virtual dispatch layer in Python to pay for).

The `@query` / `@command` declarations on the repository are read by the adapters (see
`feed.domain.semantics`): they decide which connection lane each statement rides.
"""
from typing import Any, Protocol

from ..domain.model import PostView, Principal
from ..domain.semantics import command, query


class PostRepository(Protocol):
    """Persistence of posts and likes. One SQL statement per call, exactly the spec's queries.

    Implementations must raise `PostNotFound` from `like` when the post does not exist, and must
    return `created_at` already in wire format (see `feed.domain.timefmt`).
    """

    @query
    async def newest(self, limit: int) -> list[PostView]: ...

    @query
    async def by_id(self, post_id: int) -> PostView | None: ...

    @command
    async def create(self, user_id: int, body: str) -> tuple[int, str]:
        """Insert a post, committed before returning. -> (id, created_at)."""
        ...

    @command(idempotent=True)
    async def like(self, user_id: int, post_id: int) -> bool:
        """Insert a like, committed before returning. -> True if new, False if it already existed."""
        ...

    @query
    async def ping(self) -> None:
        """`SELECT 1`; raises if the database cannot answer."""
        ...


REPOSITORY_OPERATIONS = ("newest", "by_id", "create", "like", "ping")


class Authenticator(Protocol):
    """Turns the raw Authorization header into a Principal, or raises one of the three auth errors.

    Synchronous on purpose: verification is CPU only (an HMAC and two small JSON documents), and
    the rules forbid remembering past verifications, so there is nothing to await.
    """

    def authenticate(self, authorization: str | None) -> Principal: ...


class JsonCodec(Protocol):
    """JSON in and out. Output must be compact, UTF-8, with keys in the spec's order.

    A port rather than a helper because the choice is a measurable engineering trade-off
    (hand-written f-strings, a C encoder, orjson, Pydantic models) and the use cases must not
    care which one is plugged in. `tests/test_codecs.py` proves they are byte-for-byte equal.
    """

    def post(self, view: PostView) -> bytes: ...

    def feed(self, views: list[PostView]) -> bytes: ...

    def like(self, post_id: int, already_liked: bool) -> bytes: ...

    def health(self, uptime_s: int) -> bytes: ...

    def degraded(self, error: str) -> bytes: ...

    def parse(self, raw: bytes) -> Any:
        """Decode a request body. Raises ValueError (any subclass) if it is not JSON."""
        ...
