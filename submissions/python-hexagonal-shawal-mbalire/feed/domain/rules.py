"""Input invariants. Pure functions; each one either returns a valid value or raises a DomainError."""
from .errors import BodyRequired, BodyTooLong, InvalidPostId, PostNotFound

FEED_SIZE = 20
MAX_BODY_CHARS = 500  # code points, the same unit as Postgres char_length() and Python len()

# posts.id is BIGINT. An id above this can never exist, so it is "not found", not "invalid": the
# client asked a well-formed question whose answer is no. Checking here also keeps an out-of-range
# parameter from ever reaching Postgres as an error (SQLSTATE 22003 would become a 500).
MAX_POST_ID = 2**63 - 1


def parse_post_id(raw: str) -> int:
    """`:id` must be a positive base-10 integer written with ASCII digits only.

    `str.isdigit()` alone accepts other scripts' digits ('٣') and superscripts ('²'); `isascii()`
    first rules them out, leaving exactly [0-9]+. That makes '-1', '1.5', '+1', ' 1' and '' invalid
    without a regex or a try/except on the hot path.
    """
    if not (raw.isascii() and raw.isdigit()):
        raise InvalidPostId
    post_id = int(raw)
    if post_id == 0:
        raise InvalidPostId
    if post_id > MAX_POST_ID:
        raise PostNotFound
    return post_id


def normalize_body(value: object) -> str:
    """Trim, then require 1..MAX_BODY_CHARS code points. The trimmed text is what gets stored."""
    if not isinstance(value, str):
        raise BodyRequired
    body = value.strip()
    if not body:
        raise BodyRequired
    if len(body) > MAX_BODY_CHARS:
        raise BodyTooLong
    return body
