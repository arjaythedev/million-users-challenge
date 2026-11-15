#!/usr/bin/env python3
"""Stage 3 (D and r): the CSS interprets the raw Postgres result the page hands it, then decides kind/status/error, the uptime and
the `created_at` pieces.

DOM (web/glue.js builds it from the bytes pg.js parsed; everything is a plain integer inline custom property, every
element also carries --rid), appended inside the verdict element:

    D  --e0..--e4   SQLSTATE of the ErrorResponse as 5 char codes (all 0 if there was none)
       --t0..--t15  CommandComplete tag as char codes ("INSERT 0 1", "SELECT 20", ...; 0 = absent / padding)
      r x DataRow   --i0..--i7  the int8 id column, big-endian bytes
                    --s0..--s7  the timestamptz column, big-endian bytes (int8: microseconds since 2000-01-01 UTC)
                    --l0..--l3  the int4 like_count column, big-endian bytes (0 for the INSERT ... RETURNING row)

Reads the inherited --rt, --pidh, --pidl (route stage), --cl (q), --now (unix s, on q) and --start (unix s, on #root, set once at init).
D interprets the protocol facts: the statement succeeded iff a CommandComplete tag arrived and no ErrorResponse (--dok); the
affected/returned row count is the trailing number of the tag (parsed digit by digit, --ag0..--ag15); a like of a missing post is
SQLSTATE 23503.

Outputs (`counter-reset`, exact int32):
    D: kind (1 health ok, 2 health degraded, 3 feed, 4 post, 5 created, 6 liked, 7 error, 8 already liked) st err up (uptime s)
       pid (post id, min(.., 2^31-1)) cl (close the connection after the response)
    r: id (exact int32, clamped at 2^31-1: ids >= 2^31 are out of scope) lc (like_count) dt = 1YYYYMMDD, tm = 1hhmmssmmm
       (ms TRUNCATED like Go's .000 layout; the glue strips the leading 1)

Date: the 8 timestamp bytes become three small cross-property values, each inside ONE calc() (exact doubles up to 2^53 us = year 2285):
    --dd days since 2000-01-01, --sod second of the day, --msf millisecond of the second; none exceeds 6 digits
(--dd < 10^6 days = year 4737). Neri-Schneider "Euclidean affine functions" civil-from-days (verified for every day up to 2^31 s) on days
counted from 0000-03-01 (2000-01-01 = day 730425); every intermediate that crosses a property boundary is < 1e6.
"""
from common import *

REG = Registry()
for n in ('--dok', '--dc', '--dd', '--sod', '--msf', '--cq', '--nc', '--ny'):
    REG.add(n)
for i in range(5):
    REG.add('--e%d' % i)
for i in range(16):
    REG.add('--t%d' % i)
    REG.add('--ag%d' % i)
for i in range(8):
    REG.add('--i%d' % i)
    REG.add('--s%d' % i)
for i in range(4):
    REG.add('--l%d' % i)

rt, dok, dc = (Pr(n) for n in ('--rt', '--dok', '--dc'))
now, start = Pr('--now'), Pr('--start')
cq, nc, ny = Pr('--cq'), Pr('--nc'), Pr('--ny')
dd_, sod_, msf_ = Pr('--dd'), Pr('--sod'), Pr('--msf')
E = [Pr('--e%d' % i) for i in range(5)]
T = [Pr('--t%d' % i) for i in range(16)]
AG = [Pr('--ag%d' % i) for i in range(16)]
cnt = AG[15]


def ag_expr(k):
    """number at the end of the tag, digit by digit: a digit extends the number, padding (0) keeps it, anything else resets it"""
    prev = AG[k - 1] if k else K(0)
    return If([(T[k].rng(48, 57), prev * 10 + T[k] - 48), (T[k].eq(0), prev)], 0)


# CommandComplete present and no ErrorResponse
dok_e = If([(T[0].ne(0) & E[0].eq(0), 1)], 0)
# SQLSTATE 23503 (foreign_key_violation)
fk = E[0].eq(50) & E[1].eq(51) & E[2].eq(53) & E[3].eq(48) & E[4].eq(51)

# decision case -> (kind, status, err)
CASES = [
    (1, 200, 0),   # 0 health ok
    (2, 503, 9),   # 1 health degraded
    (3, 200, 0),   # 2 feed
    (4, 200, 0),   # 3 post found
    (7, 404, 2),   # 4 post not found
    (5, 201, 0),   # 5 created
    (6, 201, 0),   # 6 liked
    (8, 200, 0),   # 7 already liked
    (7, 404, 2),   # 8 like of a missing post (foreign key violation)
    (7, 500, 9),   # 9 internal error
]


def case_expr():
    ok1 = dok.eq(1)
    return If([
        (rt.eq(1) & ok1, 0),
        (rt.eq(1), 1),
        (rt.eq(2) & ok1, 2),
        (rt.eq(3) & ok1 & cnt.eq(1), 3),
        (rt.eq(3) & ok1 & cnt.eq(0), 4),
        (rt.eq(4) & ok1, 5),
        (rt.eq(5) & ok1 & cnt.eq(1), 6),
        (rt.eq(5) & ok1 & cnt.eq(0), 7),
        (rt.eq(5) & fk, 8),
    ], 9)


kind_e = If([(dc.eq(0), 1), (dc.eq(1), 2), (dc.eq(2), 3), (dc.eq(3), 4), (dc.eq(5), 5), (dc.eq(6), 6), (dc.eq(7), 8)], 7)
st_e = If([(dc.isin(0, 2, 3, 7), 200), (dc.isin(5, 6), 201), (dc.isin(4, 8), 404), (dc.eq(1), 503)], 500)
err_e = If([(dc.isin(4, 8), 2), (dc.isin(1, 9), 9)], 0)
up_e = Max(0, now - start)
pid_e = Min(2147483647, Pr('--pidh') * 1000000 + Pr('--pidl'))
cl_e = Pr('--cl') + 0


# ---- r: id, like count, created_at ---------------------------------------------------------------------------------
def be(prefix, n):
    return [Pr('--%s%d' % (prefix, i)) for i in range(n)]


I, S, L = be('i', 8), be('s', 8), be('l', 4)
id_e = Min(2147483647, (I[0] + I[1] + I[2] + I[3]) * 4294967296 + I[4] * 16777216 + I[5] * 65536 + I[6] * 256 + I[7])
lc_e = Min(2147483647, L[0] * 16777216 + L[1] * 65536 + L[2] * 256 + L[3])
US = S[0] * (256 ** 7) + S[1] * (256 ** 6) + S[2] * (256 ** 5) + S[3] * (256 ** 4) + S[4] * 16777216 + S[5] * 65536 + S[6] * 256 + S[7]
DAY_US = 86400 * 1000000
# a negative timestamp (before 2000) is clamped to the epoch; the bytes are interpreted as unsigned otherwise
dd_e = If([(S[0].ge(128), 0)], US // DAY_US)
sod_e = If([(S[0].ge(128), 0)], Mod(US, DAY_US) // 1000000)
msf_e = If([(S[0].ge(128), 0)], Mod(US, 1000000) // 1000)


def n1():     # 4 * (days since 0000-03-01) + 3
    return 4 * (dd_ + 730425) + 3


cq_e = n1() // 146097                       # century-ish counter C
nc_e = (n1() % 146097) // 4                 # day within the century
p2 = 2939745 * (4 * nc + 3)
z_e = p2 // 4294967296
ny_e = (p2 % 4294967296) // 2939745 // 4    # day within the (March-based) year


def date_expr():
    j = ny // 306                           # 1 for January / February (ny <= 365 < 612)
    n3 = 2141 * ny + 197913
    y = 100 * cq + p2 // 4294967296 + j
    m = n3 // 65536 - 12 * j
    d = (n3 % 65536) // 2141 + 1
    return 100000000 + y * 10000 + m * 100 + d


def time_expr():
    sod = sod_
    return 1000000000 + (sod // 3600) * 10000000 + ((sod % 3600) // 60) * 100000 + (sod % 60) * 1000 + msf_


class _Id(dict):
    def __missing__(self, k): return k


def rules():
    b = _Id()
    r_ = lambda e: render(e, b)
    pre = ''.join('%s:%s;' % (n, r_(e)) for n, e in [('--ag%d' % k, ag_expr(k)) for k in range(16)] + [('--dok', dok_e), ('--dc', case_expr())])
    outs = [('kind', kind_e), ('st', st_e), ('err', err_e), ('up', up_e), ('pid', pid_e), ('cl', cl_e)]
    d = '.D{%scounter-reset:%s}\n' % (pre, ' '.join('%s %s' % (n, r_(e)) for n, e in outs))
    pre = ''.join('%s:%s;' % (n, r_(e)) for n, e in [('--dd', dd_e), ('--sod', sod_e), ('--msf', msf_e), ('--cq', cq_e), ('--nc', nc_e), ('--ny', ny_e)])
    r = '.r{%scounter-reset:id %s lc %s dt %s tm %s}\n' % (pre, r_(id_e), r_(lc_e), r_(date_expr()), r_(time_expr()))
    return d + r


def properties():
    return REG.listing()


def weak_properties():
    return [(n, '<integer>', True, 0) for n in ('--rt', '--now', '--start', '--cl', '--pidh', '--pidl')]


# ---- python reference of the date/time part (used by tests) -------------------------------------------------------
def civil(us):
    """(1YYYYMMDD, 1hhmmssmmm) for a timestamp in microseconds since 2000-01-01 UTC, evaluating the CSS expressions in Python"""
    env = {'--s%d' % i: (us >> (8 * (7 - i))) & 255 for i in range(8)}
    for n, e in (('--dd', dd_e), ('--sod', sod_e), ('--msf', msf_e), ('--cq', cq_e), ('--nc', nc_e), ('--ny', ny_e)):
        env[n] = evaluate(e, _Id2(env))
        assert 0 <= env[n] <= 999999, (n, env[n])   # nothing that crosses a property boundary may exceed 6 digits
    return evaluate(date_expr(), _Id2(env)), evaluate(time_expr(), _Id2(env))


class _Id2:
    def __init__(self, e): self.e = e
    def __getitem__(self, k): return self.e[k]


if __name__ == '__main__':
    # model check of the date arithmetic over the supported range (2000 .. year 2285, 2^53 microseconds)
    import datetime, random
    random.seed(7)
    base = datetime.datetime(2000, 1, 1)
    bad = 0
    tests = [(y, m, d) for y in (2000, 2024, 2100, 2200, 2284) for m in (2, 3, 12) for d in (1, 28, 29, 31)]
    secs = []
    for y, m, d in tests:
        try:
            secs.append(int((datetime.datetime(y, m, d) - base).total_seconds()))
        except ValueError:
            pass
    secs += [random.randrange(0, 285 * 365 * 86400) for _ in range(200000)]
    for s in secs:
        dt = base + datetime.timedelta(seconds=s)
        us = s * 1000000 + 123456
        d, t = civil(us)
        want_d = int('1%04d%02d%02d' % (dt.year, dt.month, dt.day))
        want_t = int('1%02d%02d%02d%03d' % (dt.hour, dt.minute, dt.second, 123))
        if (d, t) != (want_d, want_t):
            bad += 1
            if bad < 5: print('MISMATCH', s, d, t, want_d, want_t)
    print('stage3 date model: %d timestamps, %d mismatches' % (len(secs), bad))
    raise SystemExit(1 if bad else 0)
