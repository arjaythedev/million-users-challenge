"""The domain: what a feed *is*, independent of HTTP, JSON transports, Postgres or processes.

Ubiquitous language
-------------------
Post        an immutable message: id, body, created_at, author, like_count.
Principal   who is acting: the user id and username carried by a verified token. Never looked up.
Feed        the FEED_SIZE newest posts, ordered by (created_at DESC, id DESC).
Like        a (principal, post) pair. Idempotent: the second like is acknowledged, not counted.

Dependency rule
---------------
Nothing in this package imports from `feed.application`, `feed.adapters`, `feed.runtime` or any
third-party library. `tests/test_architecture.py` enforces it by parsing every import, so the rule
cannot rot silently. Everything here is pure: same input, same output, no I/O, no clock unless the
clock is passed in.

Modules
-------
model     the value types (Principal, PostView) and why they are tuples
rules     the invariants the spec puts on input (post id, body)
errors    the domain's failure vocabulary
semantics @query / @command: what an operation *is*, declared once, adapted by every adapter
timefmt   the timestamp contract (UTC, millisecond, truncated)
capacity  the queueing model that sizes processes and database connections
"""
