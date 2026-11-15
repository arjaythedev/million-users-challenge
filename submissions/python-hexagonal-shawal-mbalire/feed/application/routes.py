"""The API surface, declared once on the use cases, and adapted by every HTTP adapter.

    @route("POST", "/posts/{id}/like", needs=("authorization", "id"))
    async def like_post(self, authorization, raw_id): ...

`@route` records a fact about the use case (its method, path pattern and the request parts it
consumes) and returns the function unchanged. Each driving adapter then *adapts* that declaration
into its own mechanism:

    FastAPI        app.add_api_route(pattern, endpoint, methods=[method])  (its decorator, generated)
    bare ASGI      Router.match on scope["method"], scope["path"]
    granian RSGI   Router.match on scope.method, scope.path
    raw sockets    Router.match on the request line's bytes

So no adapter contains a route table of its own, and adding an endpoint is one decorated method.
The route belongs to the application rather than to an adapter because the spec defines the API
*as* HTTP (paths, status codes, exact bytes): it is the contract, not a transport detail.

Path patterns support literal segments and at most one `{id}` segment, which is all this API
needs; matching is then a dict lookup or a prefix/suffix test, never a regex.
"""
from collections.abc import Awaitable, Callable
from typing import NamedTuple

from ..domain.semantics import Semantics, semantics_of
from .contract import Reply

PARTS = ("id", "authorization", "body")
_ATTR = "__feed_route__"


class RouteSpec(NamedTuple):
    method: str
    pattern: str
    needs: tuple[str, ...]


def route(method: str, pattern: str, *, needs: tuple[str, ...] = ()):
    if pattern.count("{") > 1 or ("{" in pattern and "{id}" not in pattern):
        raise ValueError(f"unsupported pattern {pattern!r}: at most one {{id}} segment")
    if not set(needs) <= set(PARTS):
        raise ValueError(f"needs must be drawn from {PARTS}")

    def mark(fn):
        setattr(fn, _ATTR, RouteSpec(method, pattern, needs))
        return fn
    return mark


class Endpoint(NamedTuple):
    method: str
    pattern: str
    name: str
    semantics: Semantics
    wants_auth: bool
    wants_body: bool
    invoke: Callable[[str | None, str | None, bytes], Awaitable[Reply]]  # (raw_id, authorization, body)


def _invoker(handler, needs: tuple[str, ...]):
    """Precompile the argument order once, so a request pays one closure call, not a dict build."""
    picks = tuple(PARTS.index(n) for n in needs)
    if not picks:
        return lambda i, a, b: handler()
    if len(picks) == 1:
        (k,) = picks
        return lambda *x: handler(x[k])
    k0, k1 = picks
    return lambda *x: handler(x[k0], x[k1])


def endpoints(service: object) -> list[Endpoint]:
    """Every `@route`-declared method of `service`, bound. Works on decorated proxies too
    (the profiling decorator preserves the declarations via functools.wraps)."""
    found = []
    for name in dir(service):
        handler = getattr(service, name, None)
        spec = getattr(handler, _ATTR, None)
        if spec is not None:
            found.append(Endpoint(spec.method, spec.pattern, name, semantics_of(handler),
                                  "authorization" in spec.needs, "body" in spec.needs,
                                  _invoker(handler, spec.needs)))
    return found


class Router:
    __slots__ = ("endpoints", "_exact", "_param")

    def __init__(self, service: object):
        self.endpoints = endpoints(service)
        self._exact: dict[tuple[str, str], Endpoint] = {}
        self._param: list[tuple[str, str, str, Endpoint]] = []
        for e in self.endpoints:
            if "{id}" in e.pattern:
                prefix, suffix = e.pattern.split("{id}")
                self._param.append((e.method, prefix, suffix, e))
            else:
                self._exact[(e.method, e.pattern)] = e

    def match(self, method: str, path: str) -> tuple[Endpoint, str | None] | None:
        e = self._exact.get((method, path))
        if e is not None:
            return e, None
        for m, prefix, suffix, e in self._param:
            if m == method and path.startswith(prefix) and path.endswith(suffix) \
                    and len(path) >= len(prefix) + len(suffix):
                raw_id = path[len(prefix):len(path) - len(suffix)]
                if "/" not in raw_id:
                    return e, raw_id
        return None
