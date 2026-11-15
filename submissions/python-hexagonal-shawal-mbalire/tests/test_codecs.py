"""Every compliant JsonCodec must produce the same bytes for the same input: the codec is a free
choice only because this test makes it one."""
import json
import unittest

from feed.adapters.outbound.json.handwritten import HandwrittenJson

CODECS = {"handwritten": HandwrittenJson()}
for name, module, cls in [("cjson", "cjson", "CJson"), ("orjson", "orjson_codec", "OrJson"),
                          ("pydantic", "pydantic_codec", "PydanticJson")]:
    try:
        CODECS[name] = getattr(__import__(f"feed.adapters.outbound.json.{module}", fromlist=[cls]), cls)()
    except ImportError:  # extension not built / package not installed
        pass

NASTY = 'quote " backslash \\ slash / tab \t nl \n cr \r bell \x07 nul \x00 del \x7f café ✓ 😀 <b>&amp;</b>'
VIEWS = [
    (1, "Fast tomorrow small bug big droplet.", "2025-01-01T00:00:34.152Z", "happy_nebula_10241", 11),
    (500000, NASTY, "2025-12-31T23:59:19.409Z", "swift_ember_22228", 0),
    (2**62, "x" * 500, "2026-10-30T00:00:00.000Z", 'a"b', 2**31),
]


class Equivalence(unittest.TestCase):
    def test_all_codecs_registered(self):
        self.assertEqual(set(CODECS), {"handwritten", "cjson", "orjson", "pydantic"})

    def test_byte_identical(self):
        ref = CODECS["handwritten"]
        for name, codec in CODECS.items():
            with self.subTest(codec=name):
                for v in VIEWS:
                    self.assertEqual(codec.post(v), ref.post(v))
                self.assertEqual(codec.feed(VIEWS), ref.feed(VIEWS))
                self.assertEqual(codec.feed([]), ref.feed([]))
                for already in (True, False):
                    self.assertEqual(codec.like(7, already), ref.like(7, already))
                self.assertEqual(codec.health(12), ref.health(12))
                self.assertEqual(codec.degraded('no "db"'), ref.degraded('no "db"'))

    def test_reference_is_valid_json(self):
        doc = json.loads(CODECS["handwritten"].feed(VIEWS))
        self.assertEqual(doc["posts"][1]["body"], NASTY)
        self.assertEqual(list(doc["posts"][0]), ["id", "body", "created_at", "author", "like_count"])

    def test_parse_errors_are_value_errors(self):
        for name, codec in CODECS.items():
            with self.subTest(codec=name):
                self.assertEqual(codec.parse(b'{"body":"x"}'), {"body": "x"})
                for bad in (b"{bad", b"", b"\xff"):
                    with self.assertRaises(ValueError):
                        codec.parse(bad)
