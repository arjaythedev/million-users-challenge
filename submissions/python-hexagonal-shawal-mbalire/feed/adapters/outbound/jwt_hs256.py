"""Authenticator adapter: HS256 JWTs verified with the standard library.

Why not PyJWT: it is general (many algorithms, claim options, key types, a warning on short keys)
and pays for that generality on every call. This contract needs one algorithm and one claim. The
work per request is one `hmac.digest` (OpenSSL, one-shot, no HMAC object), one constant-time
compare, and two small `json.loads`. Nothing is remembered between requests (rule 9).

The signature is checked before the header or payload are even decoded, so unauthenticated bytes
never reach the JSON parser.
"""
import binascii
import hmac
import json
import time
from base64 import urlsafe_b64decode
from collections.abc import Callable

from ...domain.errors import InvalidToken, InvalidTokenPayload, MissingBearerToken
from ...domain.model import Principal


def _b64url(segment: str) -> bytes:
    return urlsafe_b64decode(segment + "=" * (-len(segment) % 4))


class Hs256Authenticator:
    __slots__ = ("_key", "_now")

    def __init__(self, secret: str, now: Callable[[], float] = time.time):
        self._key = secret.encode()
        self._now = now

    def authenticate(self, authorization: str | None) -> Principal:
        if not authorization or not authorization.startswith("Bearer "):
            raise MissingBearerToken
        token = authorization[7:]
        try:
            signing_input, _, signature = token.rpartition(".")
            header, _, payload = signing_input.partition(".")
            if not header or not payload or "." in payload:
                raise InvalidToken
            expected = hmac.digest(self._key, signing_input.encode("ascii"), "sha256")
            if not hmac.compare_digest(_b64url(signature), expected):
                raise InvalidToken
            if json.loads(_b64url(header)).get("alg") != "HS256":
                raise InvalidToken
            claims = json.loads(_b64url(payload))
            exp = claims["exp"]
            if isinstance(exp, bool) or not isinstance(exp, (int, float)) or exp <= self._now():
                raise InvalidToken
        except (ValueError, TypeError, KeyError, AttributeError, binascii.Error):
            # Non-ASCII token, bad base64, bad JSON, a header or payload that is not an object,
            # a missing exp: all of these are "not a valid token", whatever exception reports it.
            raise InvalidToken from None
        sub, username = claims.get("sub"), claims.get("username")
        if not (isinstance(sub, str) and sub.isascii() and sub.isdigit()) or not isinstance(username, str):
            raise InvalidTokenPayload
        user_id = int(sub)
        if user_id == 0:
            raise InvalidTokenPayload
        return Principal(user_id, username)
