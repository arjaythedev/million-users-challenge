"""Value types.

Why tuples and not dataclasses
------------------------------
A feed response carries 20 posts, and the server answers ~42,000 requests/s at a million users, so
roughly 400,000 post values are built per second across the box. In CPython a tuple is allocated
in one block and read by index with no attribute lookup; a slotted dataclass costs an extra
`__init__` call per instance. The database adapters can also hand back rows that *already are* this
shape (asyncpg Records are converted once; the libpq C extension builds the tuple directly), so the
core never copies a row into a second representation.

The cost is readability, recovered with `NamedTuple` for the types that are built rarely
(Principal: once per authenticated request) and with named index constants for PostView.
"""
from typing import NamedTuple


class Principal(NamedTuple):
    """The authenticated actor. `username` comes from the token as-is (the spec forbids a lookup)."""
    user_id: int
    username: str


# PostView = (id, body, created_at, author, like_count)
#   id          int, > 0
#   body        str, 1..500 code points, already trimmed
#   created_at  str, the wire timestamp (see timefmt): formatting happens once, at the adapter edge
#   author      str, users.username
#   like_count  int, rows in `likes` for this post at the time of the request
PostView = tuple[int, str, str, str, int]
ID, BODY, CREATED_AT, AUTHOR, LIKE_COUNT = range(5)
