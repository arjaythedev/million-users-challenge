#!/usr/bin/env python3
"""JWT authentication in pure CSS: the A / S / O stages of the request chain (Python 3 stdlib only).

    rules()       -> CSS text (A chunks, S blocks, O block, root HMAC midstates)
    properties()  -> [(name, syntax, inherits, initial)] for every custom property used (incl. SHA/HMAC ones)

What it computes (reference semantics: examples/go/handlers.go requireAuth + golang-jwt v5.3.1 defaults, HS256 only)
------------------------------------------------------------------------------------------------------------------
A chunks  `.A0/.A1` (value of the Authorization header, auth_chunk bytes per element, gated by
          @container style(--need-auth: 1)): one pass over every character with a per-position state chain
          (parity A0/A1 chaining; state fields `--zA<f>` / `--zB<f>`):
            * "Bearer " prefix, token = rest, trailing ASCII whitespace ignored (positions >= len behave like ' ');
            * exactly 3 dot separated base64url (RawURLEncoding: no '=', alphabet only, remainder length 1 invalid,
              trailing bits ignored) segments, decoded on the fly (a byte completes at the 2nd/3rd/4th char of a quantum);
            * a JSON state machine over the decoded bytes of segment 0 (header) and 1 (payload): full RFC 8259 grammar
              (nested objects/arrays up to depth 18, strings with all escapes incl. \\uXXXX, numbers, true/false/null),
              nothing after the top level value except whitespace (json.Unmarshal); top level must be an object
              (or `null`, which json.Unmarshal accepts and golang-jwt treats as empty claims);
            * header: last top-level "alg" must be the string "HS256" (escapes decoded);
            * payload (last duplicate key wins, like a Go map): exp / nbf (number; 0 counts as absent, null / other types
              invalid, exp required (absent / 0 -> invalid, the SPEC; Go's example parses without WithExpirationRequired),
              now < exp, nbf <= now, golang-jwt truncates NumericDate to whole seconds so only the integer part
              counts, compared with `--now`; exponent notation, fractions
              and negative numbers handled), iat ignored, sub (string: TrimSpace + ParseInt base 10 > 0, "+5" ok;
              number: plain integer > 0 and <= 2^53), username (any JSON string, literal range exported);
            * the 32 decoded signature bytes are exported as 16 big endian halves `--sf0..15` for the O stage.
          Exports (inherited): `--ap` (au assuming a valid signature: 0 no "Bearer " prefix, 1 invalid structure /
          alg / exp / nbf, 2 sub or username invalid, 3 ok), `--hm` (1 = the HMAC must be checked), `--tl` (signing input
          length L = index of the 2nd dot in the token), `--lp`, `--sf0..15`, `--au` (= ap, final when no S/O exist),
          `--uidh/--uidl`, `--un-s/--un-e`, and per position `--d<j>` (decoded payload byte completing here, else -1).
S blocks  `.S0/.S1`: inner HMAC blocks fed with the raw bytes of the auth value from offset 7. The message bytes are
          built in CSS (byte if pos < L, 0x80 if pos == L, else 0, 64 bit length in the last 8 bytes of the final
          needed block). Blocks that are not needed (or not wanted at all because the token is already known to be
          invalid) are gated off by `@container style(--nn: 1)` and cost ~nothing; they inherit the digest unchanged.
O block   `.O/.O0/.O1`: outer HMAC block (reads the digest of the last needed block whatever its parity), compares the
          16 halves with the signature and folds the result into `--au` (signature wrong -> 1).

Documented deviations from Go (everything else is meant to be exact)
  1. Leading whitespace after "Bearer " makes the token invalid (au 1): the S blocks start at offset 7.
  2. The token itself only trims ASCII whitespace; in sub strings Unicode White_Space given as \\uXXXX escapes is
     trimmed like Go, but raw UTF-8 encoded Unicode spaces (U+00A0 as C2 A0, ...) are not.
  3. JSON nesting deeper than 18 levels is rejected (Go: 10000).
  4. sub as a JSON number must be plain digits (no fraction / exponent, so 7.0 and 1e3 are rejected, Go accepts them);
     sub strings / numbers with more than 17 significant digits are rejected (Go: up to 19 for strings, 16 for numbers).
     User ids >= 1e12 are clamped to 999999999999 (the contract has two base-1e6 limbs; such a user does not exist).
  5. exp / nbf are compared on their first 10 significant digits (`now` is assumed to be 1e9 <= now < 1e10 seconds).
     Numbers >= 1e309 are rejected but the window 1.797e308..1e309 (float64 overflow in Go) is accepted; exp/nbf values
     >= 9.22e18 (Go: int64 overflow) are modelled for >= 1e19 only.
  6. Embedded CR / LF inside the token (impossible over HTTP) would be ignored by Go's base64 decoder, here invalid.
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sha256 as S

_HERE = os.path.dirname(os.path.abspath(__file__))
_K = json.load(open(os.path.join(_HERE, 'constants.json')))
CHUNK = _K['auth_chunk']
SHA_BLOCK = _K['sha_block']
assert SHA_BLOCK == 64 and CHUNK >= 8

GATE_AUTH = '@container style(--need-auth: 1)'
GATE_NN = '@container style(--nn: 1)'
GATE_HM = '@container style(--hm: 1)'

# ---------------------------------------------------------------------------------------------------------------
# tiny expression helpers: every function returns a python int (constant) or a CSS string that is safe to embed as an
# operand inside calc().
# ---------------------------------------------------------------------------------------------------------------


def V(n):
    return 'var(%s)' % n


def add(*xs):
    c = 0
    ps = []
    for x in xs:
        if isinstance(x, int):
            c += x
        else:
            ps.append(x)
    if not ps:
        return c
    if len(ps) == 1 and not c:
        return ps[0]
    t = ' + '.join(ps)
    if c > 0:
        t += ' + %d' % c
    elif c < 0:
        t += ' - %d' % -c
    return '(%s)' % t


def mul(*xs):
    c = 1
    ps = []
    for x in xs:
        if isinstance(x, int):
            c *= x
        else:
            ps.append(x)
    if c == 0:
        return 0
    if not ps:
        return c
    if c != 1:
        ps.insert(0, str(c))
    if len(ps) == 1:
        return ps[0]
    return '(%s)' % ' * '.join(ps)


def neg(x):
    return mul(-1, x)


def sub(a, b):
    return add(a, neg(b))


def dvd(a, b):
    return '(%s / %s)' % (a, b)


def fl(x):
    return 'round(down, %s)' % x


def md(a, b):
    return 'mod(%s, %s)' % (a, b)


def mn(a, b):
    return 'min(%s, %s)' % (a, b)


def mx(a, b):
    return 'max(%s, %s)' % (a, b)


def pw(a, b):
    return 'pow(%s, %s)' % (a, b)


def eq(a, b):
    if isinstance(a, int) and isinstance(b, int):
        return int(a == b)
    return 'pow(0, abs(%s))' % sub(a, b)


def ge(a, b):
    if isinstance(a, int) and isinstance(b, int):
        return int(a >= b)
    return 'clamp(0, %s, 1)' % add(sub(a, b), 1)


def gt(a, b):
    return ge(a, add(b, 1))


def le(a, b):
    return ge(b, a)


def lt(a, b):
    return ge(b, add(a, 1))


def rng(a, lo, hi):
    if isinstance(a, int):
        return int(lo <= a <= hi)
    return 'clamp(0, min(%s, %s), 1)' % (add(a, 1 - lo), sub(hi + 1, a))


def sgn(x):
    return 'clamp(-1, %s, 1)' % x


def strip_calc(e):
    e = str(e)
    if e.startswith('(') and e.endswith(')'):
        depth = 0
        for i, ch in enumerate(e):
            if ch == '(':
                depth += 1
            elif ch == ')':
                depth -= 1
                if depth == 0 and i != len(e) - 1:
                    return e
        return e[1:-1]
    return e


def calc(e):
    t = strip_calc(e)
    if isinstance(e, str):
        t = t.replace(', ', ',').replace(' * ', '*').replace(' / ', '/')
    return 'calc(%s)' % t


# ---------------------------------------------------------------------------------------------------------------
# property registry
# ---------------------------------------------------------------------------------------------------------------
_props = {}   # name -> (syntax, inherits, initial)


def reg(name, inherits=False, initial=0, syntax='<integer>'):
    old = _props.get(name)
    new = (syntax, inherits, initial)
    assert old is None or old == new, (name, old, new)
    _props[name] = new


# chain fields (inherited between chunks through `--zA<f>` / `--zB<f>`)
STATE = ['bd', 'tw', 'dn', 'ph', 'lt', 'sc', 'bc', 'lx', 'ju', 'st', 'jk', 'lr', 'kn', 'ki', 'am', 'ga',
         'sd', 'pm', 'cm', 'xv', 'xs', 'ng', 'qa', 'qb', 'qc', 'ss', 'xe', 'xn', 'sv', 'uh', 'ul', 'un', 'us', 'ue']
LOOK = ['pby', 'pbz', 'pvv']          # by[N-1], by[N-2], v[N-1] of the previous chunk
CHUNK_FIELDS = ['pb'] + ['sg%d' % k for k in range(16)]   # chunk level (not per position)
ALL_INH = STATE + LOOK + CHUNK_FIELDS

EXPECT = b'Bearer '
NOW = '--now'

# packed key table: "alg" "exp" "nbf" "sub" "username", each followed by a gap; digit = char + 1, gap = 0
_KEYLAYOUT = 'alg\0exp\0nbf\0sub\0username\0'
_KEYC = []
for _q in range(4):
    _v = 0
    for _i in range(7):
        _idx = 7 * _q + _i
        _d = 0 if _idx >= len(_KEYLAYOUT) or _KEYLAYOUT[_idx] == '\0' else ord(_KEYLAYOUT[_idx]) + 1
        _v += _d * 128 ** _i
    _KEYC.append(_v)
    assert _v < 2 ** 53
KEY_START, KEY_DEAD = 30, 100
KEY_END = {'alg': 3, 'exp': 7, 'nbf': 11, 'sub': 15, 'username': 24}


_ABLATE = set(filter(None, os.environ.get('AUTH_ABLATE', '').split(',')))   # timing experiments only


class Chunk:
    """Generator for one parity of the A chunk rule."""

    def __init__(self, parity, N):
        self.N = N
        self.inp = 'B' if parity == 0 else 'A'
        self.outp = 'A' if parity == 0 else 'B'
        self.decls = []
        self.j = 0

    # ---- names ----
    def name(self, f, j):
        N = self.N
        if f in STATE:
            if j < 0:
                return '--z%s%s' % (self.inp, f)
            if j == N - 1:
                return '--z%s%s' % (self.outp, f)
        elif f == 'by':
            if j == -1:
                return '--z%spby' % self.inp
            if j == -2:
                return '--z%spbz' % self.inp
            if j == N - 1:
                return '--z%spby' % self.outp
            if j == N - 2:
                return '--z%spbz' % self.outp
        elif f == 'v':
            if j == -1:
                return '--z%spvv' % self.inp
            if j == N - 1:
                return '--z%spvv' % self.outp
        assert j >= 0, (f, j)
        return '--z%s%d' % (f, j)

    def decl(self, f, j, expr, gate=None, other=None):
        """Declare field f at position j. With `gate` (a style() condition text) the declaration becomes
        if(gate: other; else: calc(expr)): `expr` is only evaluated when the gate is false (an idle position),
        `other` defaults to the previous value of a chain field."""
        n = self.name(f, j)
        if f in _ABLATE:
            expr = 0
        if gate is None:
            self.decls.append('%s:%s;' % (n, calc(expr)))
        else:
            if other is None:
                other = V(self.name(f, j - 1))
            self.decls.append('%s:if(%s: %s; else: %s);' % (n, gate, other, calc(expr)))
        return V(n)

    def decls_append_raw(self, f, j, value):
        n = self.name(f, j)
        self.decls.append('%s:%s;' % (n, value))
        return V(n)

    def build(self):
        for j in range(self.N):
            self.position(j)
        self.chunk_level()
        return ''.join(self.decls)

    # ---- one character position ----
    def position(self, j):
        N = self.N
        P = lambda f: V(self.name(f, j - 1))      # state before this char
        C = lambda f: V(self.name(f, j))          # value computed at this char
        D = lambda f, expr, gate=None, other=None: self.decl(f, j, expr, gate, other)
        Y = lambda f, val: 'style(%s: %s)' % (self.name(f, j), val)           # a value computed at this char
        Yp = lambda f, val: 'style(%s: %s)' % (self.name(f, j - 1), val)      # the state before this char
        off, nn_, ln = V('--off'), V('--n'), V('--len')
        cj = V('--c%d' % j)

        # -- effective char: positions >= n behave like a space (which also ends the token) --
        e = D('e', mx(cj, mul(32, sub(1, ge(nn_, j + 1)))))
        if j < 7:
            tm = D('tm', ge(off, 1))      # chars of the "Bearer " prefix are not token chars (chunk 0)
        else:
            tm = 1
        isu = sub(ge(e, 65), ge(e, 91))
        isl = sub(ge(e, 97), ge(e, 123))
        isd = sub(ge(e, 48), ge(e, 58))
        v = D('v', add(-1, mul(isu, add(e, -64)), mul(isl, add(e, -70)), mul(isd, add(e, 5)),
                       mul(eq(e, 45), 63), mul(eq(e, 95), 64)))
        cw = D('cw', mul(tm, add(eq(e, 32), rng(e, 9, 13))))
        cd = D('cd', mul(tm, eq(e, 46)))
        ck = D('ck', mul(tm, ge(v, 0)))
        co = D('co', sub(sub(sub(tm, cw), cd), ck))

        bd, tw, dn, ph, lt_, sc, bc = P('bd'), P('tw'), P('dn'), P('ph'), P('lt'), P('sc'), P('bc')
        lx, ju, st, jk, lr = P('lx'), P('ju'), P('st'), P('jk'), P('lr')
        kn, ki, am, ga = P('kn'), P('ki'), P('am'), P('ga')
        sd, pm, cm, xv, xs, ng = P('sd'), P('pm'), P('cm'), P('xv'), P('xs'), P('ng')
        qa, qb, qc, ss = P('qa'), P('qb'), P('qc'), P('ss')
        xe, xn, sv, uh, ul, un, us, ue = (P(f) for f in ('xe', 'xn', 'sv', 'uh', 'ul', 'un', 'us', 'ue'))

        # -- base64 decode: byte completing at this char (or -1) --
        pvj = V(self.name('v', j - 1))
        by = D('by', add(-1, mul(ck, add(
            mul(eq(ph, 1), add(mul(pvj, 4), fl(dvd(v, 16)), 1)),
            mul(eq(ph, 2), add(mul(md(pvj, 16), 16), fl(dvd(v, 4)), 1)),
            mul(eq(ph, 3), add(mul(md(pvj, 4), 64), v, 1))))))
        # output for glue: decoded payload byte
        self.decls.append('--d%d:%s;' % (j, calc(add(-1, mul(ge(by, 0), eq(dn, 1), add(by, 1))))))

        # -- JSON byte class (only bytes of segments 0 / 1) --
        spec = {32: 0, 9: 0, 10: 0, 13: 0, 123: 1, 125: 2, 91: 3, 93: 4, 58: 5, 44: 6, 34: 7, 92: 8, 45: 9, 48: 10,
                46: 12, 101: 13, 69: 13, 43: 14, 116: 15, 102: 16, 110: 17}
        # class of the byte (ws 0 { 1 } 2 [ 3 ] 4 : 5 , 6 " 7 \\ 8 - 9 '0' 10 '1'-'9' 11 . 12 e/E 13 + 14 t 15 f 16 n 17
        # other printable 18, control 19), -1 when there is no JSON byte here (no byte, or signature segment)
        br = ['%s: -1' % Y('by', -1), '%s: -1' % Yp('dn', 2)]
        for x, c in spec.items():
            br.append('%s: %d' % (Y('by', x), c))
        kc = self.decls_append_raw('kc', j, 'if(%s; else: %s)' % ('; '.join(br), calc(add(18, lt(by, 32), mul(rng(by, 49, 57), 11 - 18 - lt(by, 32))) if False else add(mul(rng(by, 49, 57), 11), mul(sub(1, rng(by, 49, 57)), add(18, lt(by, 32)))))))
        hx = D('hx', add(-1, mul(rng(by, 48, 57), add(by, -47)), mul(rng(by, 97, 102), add(by, -86)),
                         mul(rng(by, 65, 70), add(by, -54))), gate=Y('by', -1), other=-1)
        escmap = {34: 34, 92: 92, 47: 47, 98: 8, 102: 12, 110: 10, 114: 13, 116: 9}
        ec = D('ec', add(-1, *[mul(eq(by, x), y + 1) for x, y in escmap.items()]), gate=Y('by', -1), other=-1)

        # -- decoded char inside a string (or -1) --
        ch = D('ch', add(-1, mul(ge(kc, 0), add(
            mul(eq(lx, 1), ge(by, 32), sub(1, eq(by, 34)), sub(1, eq(by, 92)), add(by, 1, mul(65536, ge(by, 128)))),
            mul(eq(lx, 2), ge(ec, 0), add(ec, 1)),
            mul(eq(lx, 6), ge(hx, 0), add(mul(ju, 16), hx, 1))))), gate=Y('kc', -1), other=-1)
        # -- structural class: kc outside strings, 20 for the closing quote, -1 inside strings --
        sk = D('sk', add(-1, mul(ge(kc, 0), add(mul(eq(lx, 0), add(kc, 1)), mul(eq(lx, 1), eq(by, 34), 21)))), gate=Y('kc', -1), other=-1)

        # -- lexical state (string / escape / \u hex digits; 7 = error) --
        F = add(
            mul(eq(lx, 0), eq(kc, 7)),
            mul(eq(lx, 1), add(1, eq(by, 92), mul(6, lt(by, 32)), neg(eq(by, 34)))),
            mul(eq(lx, 2), add(mul(3, eq(by, 117)), ge(ec, 0), mul(7, sub(1, add(eq(by, 117), ge(ec, 0)))))),
            mul(rng(lx, 3, 6), add(mul(ge(hx, 0), add(lx, 1, mul(-6, eq(lx, 6)))), mul(7, sub(1, ge(hx, 0))))),
            mul(7, eq(lx, 7)))
        self.decl('lx', j, mul(sub(1, cd), add(lx, mul(ge(kc, 0), sub(F, lx)))), gate=Y('kc', -1) + ' and ' + Y('cd', 0))
        self.decl('ju', j, add(ju, mul(ge(kc, 0), sub(mul(rng(lx, 3, 5), ge(hx, 0), add(mul(ju, 16), hx)), ju))), gate=Y('kc', -1))

        # -- structural machine (states: 0 TOP 1 VAL 2 ARR0 3 OBJ0 4 KEY 5 COLON 6 DONE 7 AFTV 8 NZERO 9 NINT
        #    10 NFRAC 11 NEXP 12 NMINUS 13 NDOT 14 NE 15 NESGN 16 KEYSTR 17 VALSTR 18 LIT 19 ERR) --
        tt = D('tt', md(add(jk, 1), 2))
        room = lt(jk, 262143)
        pu = D('pu', mul(ge(sk, 0), add(mul(eq(sk, 1), rng(st, 0, 2), room), mul(2, eq(sk, 3), rng(st, 1, 2), room))), gate=Y('sk', -1), other=0)
        po = D('po', add(mul(eq(sk, 2), add(eq(st, 3), mul(rng(st, 7, 11), sub(1, tt)))),
                         mul(eq(sk, 4), add(eq(st, 2), mul(rng(st, 7, 11), tt)))))
        litok = eq(by, add(md(lr, 27), 96))
        S_ = st
        AF = rng(S_, 7, 11)
        V12 = rng(S_, 1, 2)
        BR = [   # (structural classes, value of the next state for that class)
            ((0,), add(19, mul(rng(S_, 0, 11), add(S_, mul(rng(S_, 8, 11), sub(7, S_)), -19)))),
            ((1,), add(19, mul(rng(S_, 0, 2), room, -16))),
            ((2,), add(19, mul(add(eq(S_, 3), mul(AF, sub(1, tt))), add(7, neg(eq(jk, 1)), -19)))),
            ((3,), add(19, mul(V12, room, -17))),
            ((4,), add(19, mul(add(eq(S_, 2), mul(AF, tt)), -12))),
            ((5,), add(19, mul(eq(S_, 5), -18))),
            ((6,), add(19, mul(AF, add(4, mul(-3, tt), -19)))),
            ((7,), add(19, mul(rng(S_, 3, 4), -3), mul(V12, -2))),
            ((9,), add(19, mul(V12, -7), mul(eq(S_, 14), -4))),
            ((14,), add(19, mul(eq(S_, 14), -4))),
            ((10,), add(19, mul(add(V12, eq(S_, 12)), -11), mul(add(eq(S_, 13), eq(S_, 10)), -9),
                         mul(eq(S_, 9), -10), mul(add(rng(S_, 14, 15), eq(S_, 11)), -8))),
            ((11,), add(19, mul(add(V12, eq(S_, 12), eq(S_, 9)), -10),
                         mul(add(eq(S_, 13), eq(S_, 10)), -9), mul(add(rng(S_, 14, 15), eq(S_, 11)), -8))),
            ((12,), add(19, mul(rng(S_, 8, 9), -6))),
            ((13,), add(19, mul(rng(S_, 8, 10), -5))),
            ((15, 16), add(19, neg(V12))),
            ((17,), add(19, neg(rng(S_, 0, 2)))),
            ((20,), add(19, mul(eq(S_, 16), -14), mul(eq(S_, 17), -12))),
        ]
        litv = add(19, mul(litok, add(-1, mul(lt(lr, 27), add(-11, neg(eq(jk, 0)))))))
        chain_ = ['%s: 0' % Y('cd', 1), '%s: %s' % (Yp('st', 18), calc(litv))]
        for cls, ex_ in BR:
            chain_.append('%s: %s' % (' or '.join(Y('sk', c) for c in cls), calc(ex_)))
        stn = 'if(%s: %s; %s; else: 19)' % (Y('sk', -1) + ' and ' + Y('cd', 0), V(self.name('st', j - 1)), '; '.join(chain_))
        self.decls.append('%s:%s;' % (self.name('st', j), stn))
        self.decl('jk', j, mul(sub(1, cd), add(jk, mul(mn(1, pu), add(jk, pu)),
                                               mul(po, sub(fl(dvd(add(jk, 1), 2)), add(jk, 1))))), gate=Y('sk', -1) + ' and ' + Y('cd', 0))
        startt = add(mul(eq(sk, 15), rng(st, 1, 2), 4230), mul(eq(sk, 16), rng(st, 1, 2), 112591),
                     mul(eq(sk, 17), rng(st, 0, 2), 9093))
        starta = add(mul(add(eq(sk, 15), eq(sk, 16)), rng(st, 1, 2)), mul(eq(sk, 17), rng(st, 0, 2)))
        self.decl('lr', j, add(lr, startt, neg(mul(starta, lr)), mul(eq(st, 18), ge(sk, 0), sub(fl(dvd(lr, 27)), lr))), gate=Y('sk', -1))

        # -- key matching --
        kf = D('kf', add(mul(eq(ch, 97), 1), mul(eq(ch, 101), 5), mul(eq(ch, 110), 9), mul(eq(ch, 115), 13),
                         mul(eq(ch, 117), 17)), gate=Y('ch', -1), other=0)
        csel = add(*[mul(eq(fl(dvd(kn, 7)), q), _KEYC[q]) for q in range(4)])
        kp = D('kp', md(fl(dvd(csel, pw(128, md(kn, 7)))), 128), gate=Y('ch', -1), other=0)
        kfv = add(KEY_DEAD, mul(ge(kf, 1), sub(kf, KEY_DEAD)))
        newk = add(mul(eq(kn, KEY_START), kfv),
                   mul(sub(1, eq(kn, KEY_START)), add(KEY_DEAD, mul(eq(add(ch, 1), kp), sub(add(kn, 1), KEY_DEAD)))))
        keyopen = mul(eq(sk, 7), rng(st, 3, 4))
        keychar = mul(eq(st, 16), ge(ch, 0))
        self.decl('kn', j, add(kn, mul(keyopen, sub(KEY_START, kn)), mul(keychar, sub(newk, kn))), gate=Y('sk', -1) + ' and (not ' + Yp('st', 16) + ')')
        keyclose = mul(eq(sk, 20), eq(st, 16))
        kid = add(mul(eq(kn, KEY_END['alg']), eq(dn, 0), 1), mul(eq(kn, KEY_END['exp']), eq(dn, 1), 2),
                  mul(eq(kn, KEY_END['nbf']), eq(dn, 1), 3), mul(eq(kn, KEY_END['sub']), eq(dn, 1), 4),
                  mul(eq(kn, KEY_END['username']), eq(dn, 1), 5))
        self.decl('ki', j, add(ki, mul(keyclose, sub(kid, ki))), gate=Y('sk', -1))

        # -- member value events at depth 1 --
        vcls = add(eq(sk, 1), eq(sk, 3), eq(sk, 7), rng(sk, 9, 11), rng(sk, 15, 17))
        vs = D('vs', mul(eq(st, 1), eq(jk, 1), vcls), gate=Y('sk', -1), other=0)                          # a top level member value starts
        sce = D('sce', mul(eq(sk, 20), eq(st, 17), eq(jk, 1)), gate=Y('sk', -1), other=0)                 # a top level string value ends
        nm = D('nm', mul(rng(st, 8, 11), add(eq(sk, 0), eq(sk, 2), eq(sk, 4), eq(sk, 6))), gate=Y('sk', -1), other=0)   # a number ends
        nmd = D('nmd', mul(nm, eq(jk, 1)), gate=Y('nm', 0), other=0)
        K = lambda i: eq(ki, i)

        # alg
        valopen = mul(eq(sk, 7), rng(st, 1, 2))
        valchar = mul(eq(st, 17), ge(ch, 0))
        chA = add(mul(eq(am, 0), 72), mul(eq(am, 1), 83), mul(eq(am, 2), 50), mul(eq(am, 3), 53), mul(eq(am, 4), 54))
        adv = add(9, mul(eq(ch, chA), sub(am, 8)))
        self.decl('am', j, add(am, mul(valopen, neg(am)), mul(valchar, sub(adv, am))), gate=Y('sk', -1) + ' and (not ' + Yp('st', 17) + ')')
        self.decl('ga', j, add(ga, mul(vs, K(1), neg(ga)), mul(sce, K(1), sub(eq(am, 5), ga))), gate=Y('vs', 0) + ' and ' + Y('sce', 0))

        # -- number tracker T1 (first digits compared with now, magnitude, sign) --
        dd = D('dd', add(-1, mul(add(eq(sk, 10), eq(sk, 11)), add(by, -47))), gate=Y('sk', -1), other=-1)
        xctx = add(rng(st, 14, 15), eq(st, 11))
        dg = D('dg', add(-1, mul(ge(dd, 0), sub(1, xctx), add(dd, 1))), gate=Y('dd', -1), other=-1)
        dx = D('dx', add(-1, mul(ge(dd, 0), xctx, add(dd, 1))), gate=Y('dd', -1), other=-1)
        stt = D('stt', mul(ge(dg, 0), mn(1, add(sd, dg))), gate=Y('dg', -1), other=0)
        t1e = D('t1e', mn(1, add(nm, ge(dg, 0), ge(dx, 0), eq(sk, 9))))     # number tracker has something to do
        ffl = add(eq(st, 13), eq(st, 10))
        live = sub(1, nm)
        self.decl('sd', j, mul(live, add(sd, stt)), gate=Y('t1e', 0))
        self.decl('pm', j, mul(live, add(pm, mul(stt, sub(1, ffl)), neg(mul(sub(ge(dg, 0), stt), ffl)))), gate=Y('t1e', 0))
        rd = md(fl(dvd(V(NOW), pw(10, sub(9, sd)))), 10)     # digit sd of now (unix seconds, 10 digits)
        self.decl('cm', j, mul(live, add(cm, mul(stt, eq(cm, 0), lt(sd, 10), sgn(sub(dg, rd))))), gate=Y('t1e', 0))
        self.decl('xv', j, mul(live, add(xv, mul(ge(dx, 0), sub(mn(add(mul(xv, 10), dx), 99999), xv)))), gate=Y('t1e', 0))
        self.decl('xs', j, mul(live, add(xs, mul(eq(sk, 9), eq(st, 14)))), gate=Y('t1e', 0))
        self.decl('ng', j, mul(live, add(ng, mul(eq(sk, 9), rng(st, 1, 2)))), gate=Y('t1e', 0))
        pt = D('pt', add(pm, xv, mul(-2, xs, xv)), gate=Y('t1e', 0), other=0)
        zero = eq(sd, 0)
        gtn = add(gt(pt, 10), mul(eq(pt, 10), eq(cm, 1)))
        huge = ge(pt, 20)      # >= 1e19 seconds: Go's int64(f) overflows to MinInt64 (far past): exp expired, nbf passed
        gtn = mul(gtn, sub(1, huge))
        expok = mul(sub(1, zero), sub(1, ng), gtn)    # exp is required (SPEC): 0 counts as absent, like golang-jwt
        verdN = mul(sub(1, zero), sub(1, ng), gtn)
        numstart = rng(sk, 9, 11)
        # xe: 0 no exp yet (invalid), 1 an exp value started (invalid until a valid number ends it), 2 valid exp
        self.decl('xe', j, add(xe, mul(vs, K(2), sub(1, xe)), mul(nmd, K(2), sub(add(1, expok), xe))), gate=Y('vs', 0) + ' and ' + Y('sce', 0) + ' and ' + Y('nmd', 0))
        self.decl('xn', j, add(xn, mul(vs, K(3), sub(sub(1, numstart), xn)), mul(nmd, K(3), sub(verdN, xn))), gate=Y('vs', 0) + ' and ' + Y('sce', 0) + ' and ' + Y('nmd', 0))
        ovf = mul(sub(1, zero), ge(pt, 310))

        # -- integer tracker T2 (sub) and the sub string parser --
        ds = D('ds', add(-1, mul(ge(dd, 0), add(rng(st, 1, 2), eq(st, 12), eq(st, 9)), add(dd, 1)),
                         mul(eq(st, 17), rng(ch, 48, 57), lt(ss, 3), add(ch, -47))), gate=Y('dd', -1) + ' and (not ' + Yp('st', 17) + ')', other=-1)
        sca = mul(eq(sk, 20), eq(st, 17))
        te = D('te', add(nm, sca), gate=Y('sk', -1), other=0)
        ca = D('ca', mul(ge(ds, 0), fl(dvd(add(mul(qa, 10), ds), 1000000))), gate=Y('ds', -1), other=0)
        cb = D('cb', mul(ge(ds, 0), fl(dvd(add(mul(qb, 10), ca), 1000000))), gate=Y('ds', -1), other=0)
        keep = sub(1, te)
        self.decl('qa', j, mul(keep, add(qa, mul(ge(ds, 0), sub(md(add(mul(qa, 10), ds), 1000000), qa)))), gate=Y('te', 0) + ' and ' + Y('ds', -1))
        self.decl('qb', j, mul(keep, add(qb, mul(ge(ds, 0), sub(md(add(mul(qb, 10), ca), 1000000), qb)))), gate=Y('te', 0) + ' and ' + Y('ds', -1))
        self.decl('qc', j, mul(keep, add(qc, mul(ge(ds, 0), sub(mn(add(mul(qc, 10), cb), 999999), qc)))), gate=Y('te', 0) + ' and ' + Y('ds', -1))
        # TrimSpace: ASCII spaces plus Unicode White_Space given as \\u escapes (raw UTF-8 bytes map to ch >= 65536)
        isw = D('iw', add(eq(ch, 32), rng(ch, 9, 13), eq(ch, 0x85), eq(ch, 0xA0), eq(ch, 0x1680), rng(ch, 0x2000, 0x200A),
                          rng(ch, 0x2028, 0x2029), eq(ch, 0x202F), eq(ch, 0x205F), eq(ch, 0x3000)), gate=Y('ch', -1), other=0)
        isp = eq(ch, 43)
        isdg = rng(ch, 48, 57)
        newss = add(4, mul(eq(ss, 0), add(mul(isw, -4), mul(isp, -3), mul(isdg, -2))), mul(eq(ss, 1), isdg, -2),
                    mul(eq(ss, 2), add(mul(isdg, -2), neg(isw))), mul(eq(ss, 3), neg(isw)))
        self.decl('ss', j, mul(sub(1, sca), add(ss, mul(eq(st, 17), ge(ch, 0), sub(newss, ss)))), gate='not ' + Yp('st', 17))
        valpos = mn(1, add(qa, qb, qc))
        strV = mul(rng(ss, 2, 3), valpos, lt(qc, 999999))
        le253 = add(lt(qc, 9007), mul(eq(qc, 9007), add(lt(qb, 199254), mul(eq(qb, 199254), le(qa, 740992)))))
        numV = mul(eq(st, 9), sub(1, ng), valpos, le253)
        self.decl('sv', j, add(sv, mul(vs, K(4), neg(sv)), mul(sce, K(4), sub(strV, sv)), mul(nmd, K(4), sub(numV, sv))), gate=Y('vs', 0) + ' and ' + Y('sce', 0) + ' and ' + Y('nmd', 0))
        endsub = mul(add(sce, nmd), K(4))
        self.decl('uh', j, add(uh, mul(endsub, sub(add(qb, mul(gt(qc, 0), sub(999999, qb))), uh))), gate=Y('vs', 0) + ' and ' + Y('sce', 0) + ' and ' + Y('nmd', 0))
        self.decl('ul', j, add(ul, mul(endsub, sub(add(qa, mul(gt(qc, 0), sub(999999, qa))), ul))), gate=Y('vs', 0) + ' and ' + Y('sce', 0) + ' and ' + Y('nmd', 0))

        # -- username literal range (decoded payload byte offsets, quotes included) --
        self.decl('us', j, add(us, mul(vs, K(5), eq(sk, 7), sub(bc, us))), gate=Y('vs', 0) + ' and ' + Y('sce', 0) + ' and ' + Y('nmd', 0))
        self.decl('un', j, add(un, mul(vs, K(5), neg(un)), mul(sce, K(5), sub(1, un))), gate=Y('vs', 0) + ' and ' + Y('sce', 0) + ' and ' + Y('nmd', 0))
        self.decl('ue', j, add(ue, mul(sce, K(5), sub(add(bc, 1), ue))), gate=Y('vs', 0) + ' and ' + Y('sce', 0) + ' and ' + Y('nmd', 0))

        # -- token level chain --
        jdone = mul(eq(st, 6), eq(lx, 0))
        lead = mul(eq(off, 0), cw) if j == 7 else 0
        self.decl('bd', j, mn(1, add(bd, mul(tw, add(cd, ck, co)), co,
                                     mul(cd, add(eq(dn, 2), 1, neg(mul(jdone, sub(1, eq(ph, 1)))))),
                                     lead, mul(nm, ovf))))
        self.decl('tw', j, mx(tw, cw), gate=Y('cw', 0))
        self.decl('dn', j, mn(2, add(dn, cd)), gate=Y('cd', 0))
        self.decl('ph', j, mul(sub(1, cd), md(add(ph, ck), 4)))
        self.decl('lt', j, add(lt_, mul(cd, eq(dn, 1), add(off, j))), gate=Y('cd', 0))
        self.decl('sc', j, add(sc, mul(ck, eq(dn, 2))), gate=Y('ck', 0))
        self.decl('bc', j, add(bc, mul(ge(by, 0), eq(dn, 1))), gate=Y('by', -1))

        # -- signature halves: index+1 of the half completing at this char (and its value) --
        mm = add(mul(3, fl(dvd(sc, 4))), ph, -1)
        D('hk', mul(ck, eq(dn, 2), ge(ph, 1), md(mm, 2), dvd(add(mm, 1), 2)))
        prevb = add(mul(eq(ph, 1), V(self.name('by', j - 2))), mul(sub(1, eq(ph, 1)), V(self.name('by', j - 1))))
        D('hv', add(by, mul(256, prevb)))

    # ---- once per chunk ----
    def chunk_level(self):
        N = self.N
        off = V('--off')
        O = lambda f: V('--z%s%s' % (self.outp, f))
        # prefix check ("Bearer ", chunk 0 only)
        pbl = add(*[mul(eq(off, 0), sub(1, eq(V('--ze%d' % j), EXPECT[j]))) for j in range(7)])
        self.decls.append('--z%spb:%s;' % (self.outp, calc(mn(1, add(V('--z%spb' % self.inp), pbl)))))
        for k in range(16):
            terms = [mul(eq(V('--zhk%d' % j), k + 1), V('--zhv%d' % j)) for j in range(N)]
            self.decls.append('--z%ssg%d:%s;' % (self.outp, k, calc(add(V('--z%ssg%d' % (self.inp, k)), *terms))))
        # ---- final verdict inputs (valid on the last chunk) ----
        struct = mul(sub(1, O('bd')), eq(O('dn'), 2), sub(1, eq(O('ph'), 1)), eq(O('sc'), 43))
        prefok = mul(sub(1, O('pb')), ge(V('--len'), 7))
        hm = mul(prefok, struct, O('ga'), eq(O('xe'), 2), sub(1, O('xn')))
        self.decls.append('--hm:%s;' % calc(hm))
        self.decls.append('--ap:%s;' % calc(mul(prefok, add(1, mul(V('--hm'), add(1, mul(O('sv'), O('un'))))))))
        self.decls.append('--au:%s;' % calc(V('--ap')))
        self.decls.append('--nn:%s;' % calc(V('--hm')))
        self.decls.append('--tl:%s;' % calc(sub(O('lt'), 7)))
        self.decls.append('--lp:%s;' % calc(md(fl(dvd(add(V('--tl'), 8), 64)), 2)))
        for k in range(16):
            self.decls.append('--sf%d:%s;' % (k, calc(O('sg%d' % k))))
        self.decls.append('--uidh:%s;' % calc(O('uh')))
        self.decls.append('--uidl:%s;' % calc(O('ul')))
        self.decls.append('--un-s:%s;' % calc(mul(O('un'), O('us'))))
        self.decls.append('--un-e:%s;' % calc(mul(O('un'), O('ue'))))


# ---------------------------------------------------------------------------------------------------------------
def _register_all():
    """Register every property that rules() uses (the registry is idempotent)."""
    N = CHUNK
    for f in ALL_INH:
        for P in 'AB':
            reg('--z%s%s' % (P, f), True)
    for p in ('--n', '--off', '--len', '--hm', '--ap', '--au'):
        pass
    for i in range(max(CHUNK, SHA_BLOCK)):
        reg('--c%d' % i)
    for p in ('--n', '--off', '--len'):
        reg(p)
    reg(NOW, True)
    for p in ('--hm', '--ap', '--au', '--tl', '--lp', '--uidh', '--uidl', '--un-s', '--un-e'):
        reg(p, True)
    for k in range(16):
        reg('--sf%d' % k, True)
    reg('--nn')
    # the generated declarations register their own non-inherited names
    for parity in (0, 1):
        ch = Chunk(parity, N)
        for d in re.split(r';(?=--)', ch.build()):
            if not d:
                continue
            n = d.split(':', 1)[0]
            assert n.startswith('--') and ' ' not in n, n
            if n.startswith('--d') and n[3:].isdigit():
                reg(n, False, -1)
            elif n not in _props:
                reg(n)
    for k in range(64):
        reg('--m%d' % k)
    for p in ('--sq', '--sfl'):
        reg(p)
    for k in range(8):
        reg('--dg%dl' % k)
        reg('--dg%dh' % k)
    for k in range(64):
        reg('--k%d' % k)


def _s_rules():
    out = []
    off, tl = V('--off'), V('--tl')
    # --nn: "my child S block is needed" (ungated, cheap)
    out.append('.S0,.S1{--nn:%s;}\n' % calc(mul(V('--hm'), ge(sub(add(tl, 8), add(off, 64)), 0))))
    d = ['--sq:%s;' % calc(sub(tl, off)), '--sfl:%s;' % calc(ge(sub(add(off, 64), add(tl, 8)), 1))]
    q = V('--sq')
    vl = mul(add(tl, 64), 8)
    lenb = [fl(dvd(vl, 16777216)), md(fl(dvd(vl, 65536)), 256), md(fl(dvd(vl, 256)), 256), md(vl, 256)]
    for i in range(64):
        e = add(mul(ge(sub(q, i), 1), V('--c%d' % i)), mul(eq(sub(q, i), 0), 128))
        if i >= 60:
            e = add(e, mul(V('--sfl'), lenb[i - 60]))
        d.append('--m%d:%s;' % (i, calc(e)))
    out.append('%s{.S0,.S1{%s}}\n' % (GATE_NN, ''.join(d)))
    out.append(S.compress_rule('.S0', 'sb', 'sa', msg=('bytes', '--m'), tag='h', wrap=GATE_NN))
    out.append(S.compress_rule('.S1', 'sa', 'sb', msg=('bytes', '--m'), tag='h', wrap=GATE_NN))
    return ''.join(out)


def _o_rules():
    out = []
    lp = V('--lp')
    d = []
    for k in range(8):
        for h in 'lh':
            d.append('--dg%d%s:%s;' % (k, h, calc(add(mul(V('--sa%d%s' % (k, h)), sub(1, lp)), mul(V('--sb%d%s' % (k, h)), lp)))))
    out.append('%s{.O,.O0,.O1{%s}}\n' % (GATE_HM, ''.join(d)))
    out.append(S.compress_rule('.O,.O0,.O1', 'ko', 'so', msg=S.digest_block_msg('dg', 96), tag='h', wrap=GATE_HM))
    diff = []
    for k in range(8):
        diff.append('abs(%s)' % sub(V('--so%dh' % k), V('--sf%d' % (2 * k))))
        diff.append('abs(%s)' % sub(V('--so%dl' % k), V('--sf%d' % (2 * k + 1))))
    sigeq = 'clamp(0, 1 - (%s), 1)' % ' + '.join(diff)
    ap = V('--ap')
    au = add(ap, mul(ge(ap, 2), sub(1, sigeq), sub(1, ap)))
    out.append('%s{.O,.O0,.O1{--au:%s;}}\n' % (GATE_AUTH, calc(au)))
    return ''.join(out)


def _root_rules():
    kb = ['var(--k%d)' % i for i in range(64)]
    out = []
    for pad, pre, tag in ((0x36, 'sb', 'ki'), (0x5c, 'ko', 'ko')):
        msg = S.words_from_byte_exprs([S.byte_xor_const(b, pad) for b in kb])
        out.append(S.compress_rule('#root', None, pre, msg=msg, tag=tag))
    return ''.join(out)


_cache = {}


def rules():
    if 'rules' not in _cache:
        out = [_root_rules()]
        a = ''.join('.A%d{%s}' % (p, Chunk(p, CHUNK).build()) for p in (0, 1))
        out.append('%s{%s}\n' % (GATE_AUTH, a))
        out.append(_s_rules())
        out.append(_o_rules())
        _cache['rules'] = ''.join(out)
    return _cache['rules']


def properties():
    rules()
    _register_all()
    res = [(n, s, i, init) for n, (s, i, init) in sorted(_props.items())]
    seen = {r[0] for r in res}
    for r in S.property_list():
        if r[0] in seen:
            old = _props[r[0]]
            assert old == (r[1], r[2], r[3]), (r, old)
        else:
            res.append(r)
    return res


def property_rules():
    return ''.join('@property %s{syntax:"%s";inherits:%s;initial-value:%s}\n'
                   % (n, s, 'true' if i else 'false', init) for n, s, i, init in properties())


if __name__ == '__main__':
    css = rules()
    pr = property_rules()
    if '--stats' in sys.argv:
        print('rules %d KB, %d properties, %d @property KB' % (len(css) // 1024, len(properties()), len(pr) // 1024))
        a = ''.join(Chunk(p, CHUNK).build() for p in (0,))
        print('one A chunk rule: %d KB, %d declarations' % (len(a) // 1024, a.count(';')))
    else:
        sys.stdout.write(css)
        sys.stdout.write(pr)
