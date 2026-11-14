"""Operation semantics: decorators with which the core *declares* what an operation is.

    @query                         reads state, changes nothing; safe to run anywhere, any time
    @command                       changes state; must be durable (committed) before it is answered
    @command(idempotent=True)      repeating it has the same effect as doing it once (a like)

These decorators do nothing at call time. They attach a `Semantics` value to the function and
return the *same* function, so a declared operation costs exactly what an undeclared one does.
They are not behaviour; they are facts that adapters translate into their own mechanisms:

    the libpq / asyncpg adapters  queries ride read lanes, commands ride write lanes
                                  (a commit's WAL flush never head-of-line-blocks a read)
    the profiling decorator       latencies are reported per kind, so read and write tails
                                  are never averaged into one meaningless number
    a retry policy (not needed    may retry a query or an idempotent command after a dropped
    here, but this is its hook)   connection, and must never retry a non-idempotent one

That is the point of declaring semantics in the domain: the *what* is stated once, inside the
hexagon, and every adapter derives its *how* from it, instead of each adapter hard-coding its
own list of which methods write.
"""
from collections.abc import Callable
from typing import NamedTuple, TypeVar

F = TypeVar("F", bound=Callable)


class Semantics(NamedTuple):
    kind: str          # QUERY or COMMAND
    idempotent: bool


QUERY = "query"
COMMAND = "command"
_ATTR = "__feed_semantics__"


def query(fn: F) -> F:
    setattr(fn, _ATTR, Semantics(QUERY, True))
    return fn


def command(fn: F | None = None, *, idempotent: bool = False):
    def mark(f: F) -> F:
        setattr(f, _ATTR, Semantics(COMMAND, idempotent))
        return f
    return mark if fn is None else mark(fn)


def is_declared(fn: Callable) -> bool:
    return hasattr(fn, _ATTR)


def semantics_of(fn: Callable) -> Semantics:
    """The declared semantics. Undeclared operations are treated as non-idempotent commands: the
    conservative default (nothing will be retried or moved off the write path by mistake)."""
    return getattr(fn, _ATTR, None) or Semantics(COMMAND, False)
