"""`Authorization: Bearer <jwt>`: the HS256 signature, `alg` and `exp` (and `nbf` if present) are checked on
every request. Nothing is remembered between requests."""
import os
from base64 import urlsafe_b64decode, urlsafe_b64encode
from hmac import compare_digest, digest

from orjson import loads

MISSING = "missing bearer token"
INVALID = "invalid or expired token"
PAYLOAD = "invalid token payload"
MAX_ID = (1 << 63) - 1  # BIGINT

_KEY = os.environ.get("JWT_SECRET", "").encode()


def _json(segment):
    return loads(urlsafe_b64decode(segment + b"=" * (-len(segment) % 4)))


def verify(authorization, now):
    """Returns (user id, username), or one of the error messages above (a str)."""
    if authorization is None or authorization[:7] != b"Bearer ":
        return MISSING
    token = authorization[7:]
    d1 = token.find(b".")
    d2 = token.find(b".", d1 + 1)
    if d1 < 0 or d2 < 0 or token.find(b".", d2 + 1) >= 0:
        return INVALID
    # The signature is compared in its base64url form: the token's third segment must be exactly the
    # unpadded encoding of HMAC-SHA256(secret, "<header>.<payload>").
    expected = urlsafe_b64encode(digest(_KEY, token[:d2], "sha256")).rstrip(b"=")
    if not compare_digest(expected, token[d2 + 1:]):
        return INVALID
    try:
        header = _json(token[:d1])
        claims = _json(token[d1 + 1:d2])
    except ValueError:  # bad base64 (binascii.Error) or bad JSON (orjson.JSONDecodeError)
        return INVALID
    if header.__class__ is not dict or header.get("alg") != "HS256" or claims.__class__ is not dict:
        return INVALID
    exp = claims.get("exp")
    if (exp.__class__ is not int and exp.__class__ is not float) or exp <= now:
        return INVALID
    nbf = claims.get("nbf")
    if nbf is not None and ((nbf.__class__ is not int and nbf.__class__ is not float) or nbf > now):
        return INVALID
    sub = claims.get("sub")
    name = claims.get("username")
    if sub.__class__ is not str or not (sub.isascii() and sub.isdigit()) or name.__class__ is not str:
        return PAYLOAD
    uid = int(sub)
    if uid <= 0 or uid > MAX_ID:
        return PAYLOAD
    return uid, name
