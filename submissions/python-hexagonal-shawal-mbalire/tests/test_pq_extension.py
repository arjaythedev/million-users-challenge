"""The C extension's timestamp decoder must agree with the domain's reference implementation."""
import unittest

from feed.domain.timefmt import iso_millis_from_pg_text

try:
    from feed.adapters.outbound.postgres import _pq
except ImportError:  # not built: pure-Python adapters can still be tested
    _pq = None


@unittest.skipIf(_pq is None, "_pq extension not built")
class Extension(unittest.TestCase):
    def test_loads(self):
        self.assertGreaterEqual(_pq.libpq_version(), 140000)  # pipeline mode needs libpq 14+

    def test_c_decoder_matches_reference(self):
        for text in ["2025-12-31 23:59:19.409123+00", "2025-12-31 23:59:19.4+00", "2025-12-31 23:59:19+00",
                     "2025-12-31 23:59:19.999999+00", "2025-01-01 00:00:00.000999+00", "2025-12-31 23:59:19.40+00"]:
            self.assertEqual(_pq.iso_millis(text), iso_millis_from_pg_text(text), text)
