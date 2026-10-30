"""Use cases against in-memory fakes: the core runs with no network, no database, no framework."""
import asyncio
import json
import unittest

from feed.adapters.outbound.json.handwritten import HandwrittenJson
from feed.adapters.outbound.jwt_hs256 import Hs256Authenticator
from feed.application.service import FeedService
from feed.domain.errors import PostNotFound
from tests.test_jwt import SECRET, sign

POST = (1, 'say "hi" \\ <b>', "2025-01-01T00:00:34.152Z", "happy_nebula_10241", 11)


class FakePosts:
    def __init__(self):
        self.likes: set[tuple[int, int]] = set()

    async def newest(self, limit):
        return [POST] * limit

    async def by_id(self, post_id):
        return POST if post_id == 1 else None

    async def create(self, user_id, body):
        return 7, "2026-10-30T00:00:00.000Z"

    async def like(self, user_id, post_id):
        if post_id != 1:
            raise PostNotFound
        new = (user_id, post_id) not in self.likes
        self.likes.add((user_id, post_id))
        return new

    async def ping(self):
        return None


def run(coro):
    return asyncio.run(coro)


class UseCases(unittest.TestCase):
    def setUp(self):
        self.service = FeedService(FakePosts(), Hs256Authenticator(SECRET), HandwrittenJson())
        self.auth = "Bearer " + sign({"sub": "1", "username": "golden_ember_1", "exp": 4102444800})

    def test_post_bytes(self):
        reply = run(self.service.post("1"))
        self.assertEqual(reply.status, 200)
        self.assertEqual(reply.body, b'{"post":{"id":1,"body":"say \\"hi\\" \\\\ <b>","created_at":"2025-01-01T00:00:34.152Z","author":"happy_nebula_10241","like_count":11}}')
        self.assertEqual(json.loads(reply.body)["post"]["body"], POST[1])

    def test_feed(self):
        reply = run(self.service.feed())
        self.assertEqual(len(json.loads(reply.body)["posts"]), 20)

    def test_errors(self):
        self.assertEqual(run(self.service.post("abc")), (400, b'{"error":"invalid post id"}'))
        self.assertEqual(run(self.service.post("2")), (404, b'{"error":"post not found"}'))
        self.assertEqual(run(self.service.like_post(None, "abc")), (401, b'{"error":"missing bearer token"}'))
        self.assertEqual(run(self.service.like_post(self.auth, "abc")), (400, b'{"error":"invalid post id"}'))
        self.assertEqual(run(self.service.create_post(self.auth, b"{bad")), (400, b'{"error":"malformed JSON body"}'))
        self.assertEqual(run(self.service.create_post(self.auth, b'{"body":123}')), (400, b'{"error":"body is required"}'))
        self.assertEqual(run(self.service.create_post(self.auth, b"[]")), (400, b'{"error":"body is required"}'))

    def test_create_and_like(self):
        reply = run(self.service.create_post(self.auth, '{"body":"  café ✓ "}'.encode()))
        self.assertEqual(reply.status, 201)
        self.assertEqual(json.loads(reply.body)["post"], {"id": 7, "body": "café ✓", "created_at": "2026-10-30T00:00:00.000Z", "author": "golden_ember_1", "like_count": 0})
        self.assertEqual(run(self.service.like_post(self.auth, "1")), (201, b'{"liked":true,"already_liked":false,"post_id":1}'))
        self.assertEqual(run(self.service.like_post(self.auth, "1")), (200, b'{"liked":true,"already_liked":true,"post_id":1}'))
        self.assertEqual(run(self.service.like_post(self.auth, "9")).status, 404)
