"""The domain's failure vocabulary.

Each failure is a type, not a string, so the application layer can map it onto the API contract
(status + message) in exactly one table (`feed.application.contract`). Adapters raise these too:
the Postgres adapters translate SQLSTATE 23503 into `PostNotFound`, the token adapter raises the
three authentication failures. The core never sees a driver exception type.

Exceptions are for the rare path only. The hot path (a valid read) raises nothing: CPython
exceptions cost microseconds each, which is affordable at 1% of traffic and not at 100%.
"""


class DomainError(Exception):
    __slots__ = ()


class InvalidPostId(DomainError):
    """The id is not a positive base-10 integer: `0`, `-1`, `abc`, `1.5`."""


class PostNotFound(DomainError):
    """No post has this id (a read, or a like whose foreign key failed)."""


class MissingBearerToken(DomainError):
    """No Authorization header, or it does not start with `Bearer `."""


class InvalidToken(DomainError):
    """Bad signature, expired, not HS256, or not a JWT at all."""


class InvalidTokenPayload(DomainError):
    """Authentic token, but `sub` is not a positive integer string or `username` is not a string."""


class MalformedJson(DomainError):
    """The request body does not parse as JSON."""


class BodyRequired(DomainError):
    """`body` is missing, not a string, or empty after trimming."""


class BodyTooLong(DomainError):
    """`body` is longer than MAX_BODY_CHARS after trimming."""
