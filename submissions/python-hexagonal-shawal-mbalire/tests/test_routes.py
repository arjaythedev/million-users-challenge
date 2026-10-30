"""The @route and @query/@command declarations, and the adapters that read them."""
import asyncio
import unittest

from feed.adapters.outbound.json.handwritten import HandwrittenJson
from feed.adapters.outbound.jwt_hs256 import Hs256Authenticator
from feed.adapters.profiling import LatencyRecorder, profiled
from feed.application.ports import REPOSITORY_OPERATIONS, PostRepository
from feed.application.routes import Router
from feed.application.service import FeedService
from feed.domain.semantics import COMMAND, QUERY, Semantics, command, query, semantics_of
from tests.test_application import FakePosts
from tests.test_jwt import SECRET


def service():
    return FeedService(FakePosts(), Hs256Authenticator(SECRET), HandwrittenJson())


class Declarations(unittest.TestCase):
    def test_decorators_return_the_same_function(self):
        def f(): ...
        self.assertIs(query(f), f)
        self.assertIs(command(f), f)
        self.assertIs(command(idempotent=True)(f), f)

    def test_repository_semantics(self):
        kinds = {op: semantics_of(getattr(PostRepository, op)) for op in REPOSITORY_OPERATIONS}
        self.assertEqual(kinds["newest"], Semantics(QUERY, True))
        self.assertEqual(kinds["create"], Semantics(COMMAND, False))
        self.assertEqual(kinds["like"], Semantics(COMMAND, True))

    def test_undeclared_is_conservative(self):
        self.assertEqual(semantics_of(lambda: None), Semantics(COMMAND, False))


class Routing(unittest.TestCase):
    def setUp(self):
        self.router = Router(service())

    def name(self, method, path):
        found = self.router.match(method, path)
        return None if found is None else (found[0].name, found[1])

    def test_the_five_endpoints(self):
        self.assertEqual(self.name("GET", "/health"), ("health", None))
        self.assertEqual(self.name("GET", "/feed"), ("feed", None))
        self.assertEqual(self.name("GET", "/posts/42"), ("post", "42"))
        self.assertEqual(self.name("GET", "/posts/abc"), ("post", "abc"))  # validation is the use case's job
        self.assertEqual(self.name("POST", "/posts"), ("create_post", None))
        self.assertEqual(self.name("POST", "/posts/abc/like"), ("like_post", "abc"))

    def test_unknown(self):
        for method, path in [("GET", "/nope"), ("GET", "/posts/1/2"), ("POST", "/posts/1/likes"),
                             ("GET", "/feed/"), ("DELETE", "/feed")]:
            self.assertIsNone(self.router.match(method, path), path)

    def test_needs(self):
        e = {e.name: e for e in self.router.endpoints}
        self.assertTrue(e["create_post"].wants_auth and e["create_post"].wants_body)
        self.assertFalse(e["post"].wants_auth or e["post"].wants_body)
        self.assertEqual(e["like_post"].semantics, Semantics(COMMAND, True))

    def test_invoke_order_auth_before_id(self):
        endpoint, raw_id = self.router.match("POST", "/posts/abc/like")
        reply = asyncio.run(endpoint.invoke(raw_id, None, b""))
        self.assertEqual(reply.status, 401)


class Profiling(unittest.TestCase):
    def test_profiled_service_still_routes_and_records(self):
        recorder = LatencyRecorder()
        proxy = profiled(service(), FeedService, "use_case", recorder)
        router = Router(proxy)
        self.assertEqual(len(router.endpoints), 5)
        endpoint, raw_id = router.match("GET", "/posts/1")
        self.assertEqual(asyncio.run(endpoint.invoke(raw_id, None, b"")).status, 200)
        self.assertEqual(sum(recorder.histograms["use_case.post [query]"]), 1)
        self.assertIn("use_case.like_post [command, idempotent]", recorder.histograms)
        self.assertIn("p95", recorder.report())

    def test_keyword_arguments_pass_through(self):
        from feed.adapters.outbound.json.handwritten import HandwrittenJson
        from feed.application.ports import JsonCodec
        codec = profiled(HandwrittenJson(), JsonCodec, "json", LatencyRecorder())
        self.assertEqual(codec.like(1, already_liked=True), HandwrittenJson().like(1, True))
