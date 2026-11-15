"""Driving adapter (app): a bare ASGI 3 application. No web framework: ~40 lines over the Router.

Runs under any ASGI server (uvicorn, granian). Compared with the FastAPI app on the same server,
it isolates what the framework layer itself costs per request (routing, dependency injection,
the Request/Response objects, the middleware stack).
"""
from ...application.contract import INTERNAL_ERROR, NOT_FOUND
from ...application.routes import Router
from . import policy

_JSON = (b"content-type", b"application/json")
_GZIP = (b"content-encoding", b"gzip")
_VARY = (b"vary", b"accept-encoding")


def build(router: Router):
    match = router.match

    async def app(scope, receive, send):
        if scope["type"] != "http":  # lifespan: the composition root already did startup
            message = await receive()
            if message["type"] == "lifespan.startup":
                await send({"type": "lifespan.startup.complete"})
            return
        authorization = None
        accepts_gzip = False
        for name, value in scope["headers"]:
            if name == b"authorization":
                authorization = value.decode("latin-1")
            elif name == b"accept-encoding":
                accepts_gzip = b"gzip" in value
        found = match(scope["method"], scope["path"])
        if found is None:
            reply = NOT_FOUND
        else:
            endpoint, raw_id = found
            body = b""
            if endpoint.wants_body:
                more = True
                while more:
                    message = await receive()
                    body += message.get("body", b"")
                    more = message.get("more_body", False)
            try:
                reply = await endpoint.invoke(raw_id, authorization, body)
            except Exception:
                reply = INTERNAL_ERROR
        payload, gzipped = policy.encode(reply.body, accepts_gzip)
        headers = [_JSON, (b"content-length", str(len(payload)).encode())]
        if gzipped:
            headers += (_GZIP, _VARY)
        await send({"type": "http.response.start", "status": reply.status, "headers": headers})
        await send({"type": "http.response.body", "body": payload})

    return app
