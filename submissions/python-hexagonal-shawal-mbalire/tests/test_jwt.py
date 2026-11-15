import base64
import hashlib
import hmac
import json
import unittest

from feed.adapters.outbound.jwt_hs256 import Hs256Authenticator
from feed.domain.errors import InvalidToken, InvalidTokenPayload, MissingBearerToken

SECRET = "million-users-challenge"
NOW = 1_800_000_000


def b64(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def sign(claims: dict, header: dict | None = None, secret: str = SECRET) -> str:
    signing_input = b64(json.dumps(header or {"alg": "HS256", "typ": "JWT"}).encode()) + "." + b64(json.dumps(claims).encode())
    return signing_input + "." + b64(hmac.digest(secret.encode(), signing_input.encode(), hashlib.sha256))


class Hs256(unittest.TestCase):
    def setUp(self):
        self.auth = Hs256Authenticator(SECRET, now=lambda: NOW)
        self.claims = {"sub": "1", "username": "golden_ember_1", "iat": NOW, "exp": NOW + 3600}

    def check(self, header, error):
        with self.assertRaises(error):
            self.auth.authenticate(header)

    def test_valid(self):
        p = self.auth.authenticate("Bearer " + sign(self.claims))
        self.assertEqual((p.user_id, p.username), (1, "golden_ember_1"))

    def test_missing(self):
        self.check(None, MissingBearerToken)
        self.check("Basic abc", MissingBearerToken)
        self.check("bearer x", MissingBearerToken)

    def test_invalid(self):
        good = sign(self.claims)
        self.check("Bearer abc", InvalidToken)
        self.check("Bearer " + good.rsplit(".", 1)[0] + ".c2lnbmF0dXJl", InvalidToken)
        self.check("Bearer " + sign(self.claims, secret="other"), InvalidToken)
        self.check("Bearer " + b64(b'{"alg":"none","typ":"JWT"}') + "." + good.split(".")[1] + ".", InvalidToken)
        self.check("Bearer " + sign(self.claims, header={"alg": "HS512"}), InvalidToken)
        self.check("Bearer " + sign({**self.claims, "exp": NOW - 1}), InvalidToken)
        self.check("Bearer " + sign({**self.claims, "exp": NOW}), InvalidToken)
        self.check("Bearer " + sign({k: v for k, v in self.claims.items() if k != "exp"}), InvalidToken)
        self.check("Bearer " + sign({**self.claims, "exp": "soon"}), InvalidToken)
        self.check("Bearer " + sign([1, 2]), InvalidToken)
        self.check("Bearer é.é.é", InvalidToken)
        self.check("Bearer " + good + ".extra", InvalidToken)

    def test_payload(self):
        self.check("Bearer " + sign({**self.claims, "sub": "abc"}), InvalidTokenPayload)
        self.check("Bearer " + sign({**self.claims, "sub": 1}), InvalidTokenPayload)
        self.check("Bearer " + sign({**self.claims, "sub": "0"}), InvalidTokenPayload)
        self.check("Bearer " + sign({k: v for k, v in self.claims.items() if k != "username"}), InvalidTokenPayload)
