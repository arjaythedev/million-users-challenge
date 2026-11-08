"""Feed API: a FastAPI app on Gunicorn + uvicorn workers, with asyncpg.

The FastAPI app object, its router and lifespan run every request. What is cut is per-request
work the API does not need: endpoints are raw ASGI apps on the router, so a request skips
dependency solving, Pydantic models, the Request/Response objects and the middleware stack.
JSON is orjson, the JWT is checked by hand (HS256 signature and exp, every request), gzip is
ISA-L, and Postgres sends timestamps as text (UTC session), so no datetime objects are made.
"""
import base64
import gc
import hashlib
import hmac
import os
import re
import time
from contextlib import asynccontextmanager

import asyncpg
import orjson
from isal import isal_zlib
from fastapi import FastAPI
from starlette.exceptions import HTTPException as StarletteHTTPException
from starlette.routing import Route

from .db import ACQUIRE_TIMEOUT_S, FEED_SIZE, FEED_SQL, INSERT_LIKE_SQL, INSERT_POST_SQL, POST_SQL, Pool, create_pool

STARTED_AT = time.monotonic()
JWT_SECRET = os.environ["JWT_SECRET"].encode()
MAX_BODY_LENGTH = 500
MAX_ID = 2**63 - 1  # BIGINT; a larger id cannot exist
GZIP_MIN_SIZE = 500  # same threshold as the GZipMiddleware default in examples/python
pool: Pool  # one per worker process, created in lifespan


# --- responses -------------------------------------------------------------------------------

class JSONResponse:
    """Two ASGI sends. Gzips (ISA-L) when the client accepts it and the body is big enough."""

    __slots__ = ("status", "body")

    def __init__(self, status: int, body: bytes):
        self.status = status
        self.body = body

    async def __call__(self, scope, receive, send):
        body = self.body
        headers = [(b"content-type", b"application/json")]
        if len(body) >= GZIP_MIN_SIZE and accepts_gzip(scope):
            body = isal_zlib.compress(body, 0, 31)  # ISA-L level 0 (its fastest); wbits 31 = gzip container
            headers.append((b"content-encoding", b"gzip"))
            headers.append((b"vary", b"Accept-Encoding"))
        headers.append((b"content-length", str(len(body)).encode()))
        await send({"type": "http.response.start", "status": self.status, "headers": headers})
        await send({"type": "http.response.body", "body": body})


def header(scope, name: bytes):
    for k, v in scope["headers"]:
        if k == name:
            return v
    return None


def accepts_gzip(scope) -> bool:
    value = header(scope, b"accept-encoding")
    return value is not None and b"gzip" in value


def json_response(status: int, data) -> JSONResponse:
    return JSONResponse(status, orjson.dumps(data))


def error(status: int, message: str) -> JSONResponse:
    return json_response(status, {"error": message})


def post_object(row) -> dict:
    return {"id": row[0], "body": row[1], "created_at": row[2], "author": row[3], "like_count": row[4]}


def parse_id(raw: str):
    """A positive integer in plain decimal digits, else None."""
    if not (raw.isascii() and raw.isdigit()):
        return None
    n = int(raw)
    return n if n > 0 else None


async def read_body(receive) -> bytes:
    chunks = []
    while True:
        message = await receive()
        chunks.append(message.get("body", b""))
        if not message.get("more_body"):
            return b"".join(chunks)


# --- auth ------------------------------------------------------------------------------------

B64URL = re.compile(r"[A-Za-z0-9_-]*")


def b64url_decode(s: str) -> bytes:
    if not B64URL.fullmatch(s):
        raise ValueError("not base64url")
    return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))


def authenticate(scope):
    """Returns (user_id, username) or an error JSONResponse. HS256 only; signature and exp checked."""
    raw = header(scope, b"authorization")
    value = raw.decode("latin-1") if raw is not None else None
    if value is None or not value.startswith("Bearer "):
        return error(401, "missing bearer token")
    try:
        head_b64, payload_b64, sig_b64 = value[7:].split(".")
        head = orjson.loads(b64url_decode(head_b64))
        if not isinstance(head, dict) or head.get("alg") != "HS256":
            raise ValueError("alg")
        digest = hmac.new(JWT_SECRET, f"{head_b64}.{payload_b64}".encode(), hashlib.sha256).digest()
        if not hmac.compare_digest(base64.urlsafe_b64encode(digest).rstrip(b"="), sig_b64.encode()):
            raise ValueError("signature")
        payload = orjson.loads(b64url_decode(payload_b64))
        if not isinstance(payload, dict):
            raise ValueError("payload")
        exp = payload.get("exp")  # required, like PyJWT with options={"require": ["exp"]}
        if not isinstance(exp, (int, float)) or isinstance(exp, bool) or time.time() >= exp:
            raise ValueError("exp")
    except (ValueError, UnicodeError):  # includes orjson.JSONDecodeError and binascii.Error
        return error(401, "invalid or expired token")
    sub, username = payload.get("sub"), payload.get("username")
    user_id = parse_id(sub) if isinstance(sub, str) else None
    if user_id is None or user_id > MAX_ID or not isinstance(username, str):
        return error(401, "invalid token payload")
    return user_id, username


# --- app -------------------------------------------------------------------------------------

@asynccontextmanager
async def lifespan(app: FastAPI):
    global pool
    pool = await create_pool()  # each Gunicorn worker owns its pool on its own event loop
    # Each worker holds thousands of keep-alive connections, each made of many objects, and a full
    # GC pass walks all of them: a pause of that length is a p99 spike. Request handling creates
    # almost no reference cycles, so freeze what exists now and run the older generations rarely.
    gc.collect()
    gc.freeze()
    gc.set_threshold(50_000, 50, 1000)
    yield
    await pool.close()


app = FastAPI(lifespan=lifespan, docs_url=None, redoc_url=None, openapi_url=None,
              telemetry={"tracing": False, "metrics": False, "logs": False})


async def not_found(scope, receive, send):
    await error(404, "not found")(scope, receive, send)


app.router.default = not_found


def _slim_stack():
    """The router plus one try/except that keeps every error in the API's JSON format."""
    router = app.router

    async def stack(scope, receive, send):
        if scope["type"] != "http":
            await router(scope, receive, send)  # lifespan
            return
        try:
            await router(scope, receive, send)
        except StarletteHTTPException as exc:  # 405 from the router
            await error(exc.status_code, str(exc.detail).lower())(scope, receive, send)
        except Exception:
            await error(500, "internal server error")(scope, receive, send)

    return stack


app.build_middleware_stack = _slim_stack


class Endpoint:
    """Starlette mounts a non-function endpoint as a raw ASGI app: no Request, no wrapper."""

    __slots__ = ("handler",)

    def __init__(self, handler):
        self.handler = handler

    async def __call__(self, scope, receive, send):
        await (await self.handler(scope, receive))(scope, receive, send)


def route(path, methods):
    def add(endpoint):
        app.router.routes.append(Route(path, Endpoint(endpoint), methods=methods))
        return endpoint
    return add


# The router tries routes in order, so the two hot reads come first (the load is about half
# GET /feed and half GET /posts/:id) and /health last.
@route("/feed", methods=["GET"])
async def feed(scope, receive):
    async with pool.acquire() as conn:
        rows = await conn.fetch(FEED_SQL, FEED_SIZE)
    return json_response(200, {"posts": [post_object(r) for r in rows]})


@route("/posts/{post_id}", methods=["GET"])
async def get_post(scope, receive):
    pid = parse_id(scope["path_params"]["post_id"])
    if pid is None:
        return error(400, "invalid post id")
    if pid > MAX_ID:
        return error(404, "post not found")
    async with pool.acquire() as conn:
        row = await conn.fetchrow(POST_SQL, pid)
    if row is None:
        return error(404, "post not found")
    return json_response(200, {"post": post_object(row)})


@route("/posts", methods=["POST"])
async def create_post(scope, receive):
    user = authenticate(scope)
    if isinstance(user, JSONResponse):
        return user
    user_id, username = user
    try:
        data = orjson.loads(await read_body(receive))
    except orjson.JSONDecodeError:
        return error(400, "malformed JSON body")
    body = data.get("body") if isinstance(data, dict) else None
    if not isinstance(body, str):
        return error(400, "body is required")
    body = body.strip()
    if not body:
        return error(400, "body is required")
    if len(body) > MAX_BODY_LENGTH:
        return error(400, f"body must be at most {MAX_BODY_LENGTH} characters")
    async with pool.acquire() as conn:
        post_id, created_at = await conn.fetchrow(INSERT_POST_SQL, user_id, body)
    return json_response(201, {"post": {"id": post_id, "body": body, "created_at": created_at,
                                        "author": username, "like_count": 0}})


@route("/posts/{post_id}/like", methods=["POST"])
async def like_post(scope, receive):
    user = authenticate(scope)  # auth before the id, per the spec
    if isinstance(user, JSONResponse):
        return user
    pid = parse_id(scope["path_params"]["post_id"])
    if pid is None:
        return error(400, "invalid post id")
    if pid > MAX_ID:
        return error(404, "post not found")
    try:
        async with pool.acquire() as conn:
            status = await conn.execute(INSERT_LIKE_SQL, user[0], pid)
    except asyncpg.ForeignKeyViolationError:  # SQLSTATE 23503: no such post
        return error(404, "post not found")
    inserted = status == "INSERT 0 1"
    return json_response(201 if inserted else 200, {"liked": True, "already_liked": not inserted, "post_id": pid})


@route("/health", methods=["GET"])
async def health(scope, receive):
    try:
        async with pool.acquire(timeout=ACQUIRE_TIMEOUT_S) as conn:
            await conn.fetchval("SELECT 1")
    except Exception as exc:
        return json_response(503, {"status": "degraded", "db": "unreachable", "error": str(exc)})
    return json_response(200, {"status": "ok", "db": "ok", "uptime_s": int(time.monotonic() - STARTED_AT)})
