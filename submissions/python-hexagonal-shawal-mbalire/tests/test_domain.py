import unittest
from datetime import datetime, timedelta, timezone

from feed.domain import capacity, rules
from feed.domain.errors import BodyRequired, BodyTooLong, InvalidPostId, PostNotFound
from feed.domain.timefmt import iso_millis, iso_millis_from_pg_text


class PostId(unittest.TestCase):
    def test_valid(self):
        self.assertEqual(rules.parse_post_id("1"), 1)
        self.assertEqual(rules.parse_post_id("500000"), 500000)

    def test_invalid(self):
        for raw in ["0", "-1", "abc", "1.5", "", "+1", " 1", "٣", "²", "0x10", "1e3"]:
            with self.assertRaises(InvalidPostId, msg=raw):
                rules.parse_post_id(raw)

    def test_beyond_bigint_is_not_found(self):
        with self.assertRaises(PostNotFound):
            rules.parse_post_id(str(2**63))


class Body(unittest.TestCase):
    def test_trimmed(self):
        self.assertEqual(rules.normalize_body("  hi \n"), "hi")

    def test_required(self):
        for value in [None, 123, "", "   ", ["x"]]:
            with self.assertRaises(BodyRequired):
                rules.normalize_body(value)

    def test_length_in_code_points(self):
        self.assertEqual(len(rules.normalize_body("é" * 500)), 500)
        with self.assertRaises(BodyTooLong):
            rules.normalize_body("a" * 501)


class Timestamps(unittest.TestCase):
    def test_truncates_not_rounds(self):
        dt = datetime(2025, 12, 31, 23, 59, 59, 999_999, tzinfo=timezone.utc)
        self.assertEqual(iso_millis(dt), "2025-12-31T23:59:59.999Z")

    def test_converts_to_utc(self):
        dt = datetime(2026, 1, 1, 2, 0, 0, 409_123, tzinfo=timezone(timedelta(hours=2)))
        self.assertEqual(iso_millis(dt), "2026-01-01T00:00:00.409Z")

    def test_pg_text(self):
        cases = {
            "2025-12-31 23:59:19.409123+00": "2025-12-31T23:59:19.409Z",
            "2025-12-31 23:59:19.4+00": "2025-12-31T23:59:19.400Z",
            "2025-12-31 23:59:19+00": "2025-12-31T23:59:19.000Z",
            "2025-12-31 23:59:19.999999+00": "2025-12-31T23:59:19.999Z",
        }
        for text, wire in cases.items():
            self.assertEqual(iso_millis_from_pg_text(text), wire)


class Capacity(unittest.TestCase):
    def test_million_users_load(self):
        self.assertAlmostEqual(capacity.offered_load(1_000_000), 41_666.7, places=0)

    def test_lanes_respect_cap(self):
        for workers in [1, 4, 8, 16, 64, 255]:
            per = capacity.read_lanes_per_worker(workers)
            self.assertLessEqual(workers * (per + capacity.WRITE_LANES_PER_WORKER), capacity.POSTGRES_CONNECTION_CAP)
            self.assertGreaterEqual(per, 1)

    def test_sixteen_cores(self):
        self.assertEqual(capacity.read_lanes_per_worker(16), 3)
