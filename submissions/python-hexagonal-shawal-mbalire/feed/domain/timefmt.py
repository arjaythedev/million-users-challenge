"""The timestamp contract: `YYYY-MM-DDTHH:MM:SS.mmmZ`, in UTC, with the milliseconds **truncated**.

Why truncate, not round
-----------------------
`created_at` is a TIMESTAMPTZ with microsecond resolution. Rounding 23:59:59.9996 up would carry
into the next second, minute, even the next year, and a post would then appear to have been written
after a later post that rounded down. Truncation is monotonic: if a <= b then trunc(a) <= trunc(b),
so the wire order always agrees with the feed's ORDER BY created_at. (It is also what JavaScript's
Date does, which is how the golden files were produced.)

Two encodings, one contract
---------------------------
Drivers hand timestamps over in different shapes: asyncpg decodes the binary protocol into an aware
`datetime`; libpq in text mode gives `2025-12-31 23:59:19.409123+00` once the session runs with
TimeZone=UTC and DateStyle=ISO. Both are converted here, at the edge, exactly once, and everything
inward of the adapters only ever sees the finished string. The libpq C extension re-implements
`iso_millis_from_pg_text` in C for speed; `tests/test_timefmt.py` checks the two agree.
"""
from datetime import datetime, timezone


def iso_millis(value: datetime) -> str:
    value = value.astimezone(timezone.utc)
    return f"{value:%Y-%m-%dT%H:%M:%S}.{value.microsecond // 1000:03d}Z"


def iso_millis_from_pg_text(text: str) -> str:
    """`2025-12-31 23:59:19.409123+00` -> `2025-12-31T23:59:19.409Z`.

    Postgres drops trailing zeros from the fraction (`.4+00`) and omits it entirely on a whole
    second (`19+00`), so the fraction is right-padded to three digits rather than sliced blindly.
    Assumes a UTC session: the offset is ignored, not applied.
    """
    fraction = ""
    if len(text) > 19 and text[19] == ".":
        i = 20
        while i < len(text) and i < 23 and text[i].isdigit():
            i += 1
        fraction = text[20:i]
    return f"{text[:10]}T{text[11:19]}.{fraction:0<3}Z"
