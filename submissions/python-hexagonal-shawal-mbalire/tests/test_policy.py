"""Transport policy: what gets compressed, and that it decodes as standard gzip at any size."""
import gzip
import json
import unittest

from feed.adapters.inbound import _deflate, policy

COMPRESSORS = {"zlib": policy.zlib_gzip, "libdeflate": _deflate.gzip}


def feed_body(posts: int, body_len: int) -> bytes:
    return json.dumps({"posts": [
        {"id": 500_000 - i, "body": f"post {i} " + "x" * body_len, "created_at": "2026-10-30T17:05:55.172Z",
         "author": f"lazy_pebble_{i}", "like_count": i % 7} for i in range(posts)]}, separators=(",", ":")).encode()


class GzipPolicyTest(unittest.TestCase):
    def tearDown(self):
        policy.use_gzip(policy.zlib_gzip)

    def test_every_compressor_round_trips_at_and_beyond_the_window(self):
        for name, compress in COMPRESSORS.items():
            policy.use_gzip(compress)
            for body in (feed_body(20, 60), feed_body(20, 2_000), feed_body(200, 300)):  # ~3.6 to ~70 KB
                with self.subTest(compressor=name, size=len(body)):
                    wire, gzipped = policy.encode(body, accepts_gzip=True)
                    self.assertTrue(gzipped)
                    self.assertEqual(wire[:2], b"\x1f\x8b")  # the gzip container, not raw zlib
                    self.assertEqual(gzip.decompress(wire), body)
                    self.assertLess(len(wire), len(body) // 2)

    def test_libdeflate_accepts_any_buffer_and_empty_input(self):
        self.assertEqual(gzip.decompress(_deflate.gzip(b"")), b"")
        self.assertEqual(gzip.decompress(_deflate.gzip(bytearray(b"abc" * 500))), b"abc" * 500)
        with self.assertRaises(TypeError):
            _deflate.gzip("not bytes")

    def test_small_bodies_and_non_gzip_clients_pass_through(self):
        small = b'{"id":1}'
        self.assertEqual(policy.encode(small, accepts_gzip=True), (small, False))
        big = feed_body(20, 60)
        self.assertEqual(policy.encode(big, accepts_gzip=False), (big, False))


if __name__ == "__main__":
    unittest.main()
