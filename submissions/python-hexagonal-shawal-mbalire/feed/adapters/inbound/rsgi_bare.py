"""Driving adapter (app): granian's native RSGI interface. No web framework, and no ASGI either.

ASGI turns every request into dicts and every response into two `send` awaits. RSGI hands the app
a scope object whose headers are read on demand in Rust, and takes the response in one
synchronous call (`response_bytes`). This is the thinnest Python surface any server here offers.
"""
from ...application.contract import INTERNAL_ERROR, NOT_FOUND
from ...application.routes import Router
from . import policy

_JSON = ("content-type", "application/json")
_GZIP = [("content-encoding", "gzip"), ("vary", "accept-encoding")]


class RsgiApp:
    __slots__ = ("_match",)

    def __init__(self, router: Router):
        self._match = router.match

    async def __rsgi__(self, scope, protocol):
        if scope.proto != "http":
            return
        headers = scope.headers
        found = self._match(scope.method, scope.path)
        if found is None:
            reply = NOT_FOUND
        else:
            endpoint, raw_id = found
            authorization = headers.get("authorization") if endpoint.wants_auth else None
            body = await protocol() if endpoint.wants_body else b""
            try:
                reply = await endpoint.invoke(raw_id, authorization, body)
            except Exception:
                reply = INTERNAL_ERROR
        payload, gzipped = policy.encode(reply.body, "gzip" in (headers.get("accept-encoding") or ""))
        response_headers = [_JSON, ("content-length", str(len(payload)))]
        if gzipped:
            response_headers += _GZIP
        protocol.response_bytes(reply.status, response_headers, payload)

    def __rsgi_init__(self, loop):
        pass

    def __rsgi_del__(self, loop):
        pass


def build(router: Router) -> RsgiApp:
    return RsgiApp(router)
