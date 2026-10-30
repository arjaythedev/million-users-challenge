"""Driving adapter (app): FastAPI, generated from the application's `@route` declarations.

The core's decorators are *adapted* into FastAPI's: for every declared endpoint this calls
`app.add_api_route(pattern, endpoint, methods=[method])`, which is exactly what `@app.get(...)`
does under the hood. No route is written here by hand.

FastAPI's own validation is deliberately bypassed: the path id arrives as a raw string and the
body as raw bytes, because the order of checks (auth before id) and the exact error bytes are the
application's contract, not the framework's. Responses are the core's bytes, passed through.
"""
from fastapi import FastAPI, Request
from fastapi.middleware.gzip import GZipMiddleware
from fastapi.responses import Response
from starlette.exceptions import HTTPException

from ...application.contract import INTERNAL_ERROR, NOT_FOUND, Reply
from ...application.routes import Endpoint, Router
from .policy import GZIP_LEVEL, GZIP_MIN_BYTES


def _response(reply: Reply) -> Response:
    return Response(reply.body, reply.status, media_type="application/json")


def _adapt(endpoint: Endpoint):
    invoke, wants_auth, wants_body = endpoint.invoke, endpoint.wants_auth, endpoint.wants_body

    async def handler(request: Request) -> Response:
        authorization = request.headers.get("authorization") if wants_auth else None
        body = await request.body() if wants_body else b""
        return _response(await invoke(request.path_params.get("id"), authorization, body))

    handler.__name__ = endpoint.name
    return handler


def build(router: Router) -> FastAPI:
    app = FastAPI(docs_url=None, redoc_url=None, openapi_url=None)
    app.add_middleware(GZipMiddleware, minimum_size=GZIP_MIN_BYTES, compresslevel=GZIP_LEVEL)

    for endpoint in router.endpoints:
        app.add_api_route(endpoint.pattern, _adapt(endpoint), methods=[endpoint.method])

    @app.exception_handler(HTTPException)
    async def unrouted(request: Request, exc: HTTPException) -> Response:
        return _response(NOT_FOUND)

    @app.exception_handler(Exception)
    async def crashed(request: Request, exc: Exception) -> Response:
        return _response(INTERNAL_ERROR)

    return app
