#!/usr/bin/env python3
"""R stage: routing + `:id` parsing over the raw request-target bytes (path, optionally followed by `?query`).

DOM (src/fragment.rs): one element per `path_chunk` (8) bytes, class `R Rf` (first), then `R R1`, `R R0`, `R R1`, ...; there
are always at least two (the second may be empty), so that the first chunk can stay tiny.
Inline props per chunk: --rid, --n, --off, --len, --c0.. (bytes; unused positions are 0, a byte that cannot occur in a
request target, so 0 doubles as "padding" and no --n masking is needed); the first chunk also --mt (0 GET, 1 POST, 2 other).

Semantics (gin, see examples/go/handlers.go): exact routes GET /health, GET /feed, POST /posts, GET /posts/:id,
POST /posts/:id/like; `?query` ignored; anything else (wrong method, trailing slash, empty id segment, ...) -> rt 0.
`:id` = ParseInt(s, 10, 64) > 0 semantics: optional leading '+', >= 1 digit, leading zeros ok, value > 0, <= 2^63-1.

Cost model (measured): evaluating a declaration costs ~7 ns per byte of its text plus ~0.6 us per var()/style() evaluated, so
the hot paths must be small. The first chunk (Rf) only decides the literal prefix (/health /feed /posts, the follow byte
at index 6/7) in parallel and outputs --rpm; only for `/posts/...` (--rpm 1) the later chunks (gated by
@container style(--rpm: 1)) run the per-byte automaton over the id.

Mode automaton (one registered integer per byte position):
    0 dead (absorbing)           41 /health[?..]   42 /feed[?..]   43 /posts/<id>[?..] (GET)   44 /posts[?..] (POST)
    1 id segment, empty          45 /posts/<id>/like[?..] (POST)   5x = the same with an invalid id
    2 id segment "+" so far      5..9 after "/posts/<id>/", "l", "li", "lik", "like"   (+10: id invalid)
    3 id segment, >= 1 digit     11 id segment, invalid
Modes >= 40 are absorbing ("route decided, the rest is a query string").

Id value: Horner over the digit run of each chunk inside ONE calc() (exact doubles); across chunks base-1e6 limbs
(l0, l1, l2 = value mod 1e6, / 1e6 mod 1e6, / 1e12 capped), so ids < 1e12 are exact in (--pidh, --pidl) = (value div 1e6,
value mod 1e6); beyond that --pidh loses digits (ids >= 2^53 are out of scope). A value >= 2^63 (as a double) is invalid.

Outputs (inherited): --rt (0 unknown, 1 health, 2 feed, 3 get post, 4 create, 5 like), --idok, --pidh, --pidl,
--need-auth, --need-body.
"""
from common import *

REG = Registry()
S = constants()['path_chunk']
assert S == 8, 'path_chunk must be 8: the first chunk decides the literal prefix and its follow byte'

LIM = 1000000


def P(n): return Pr(n)


mt, ln_ = P('--mt'), P('--len')

# ------------------------------------------------------------------------------------------------ registrations
for i in range(S):
    REG.add('--c%d' % i)
for n in ('--n', '--off', '--len'):
    REG.add(n)
REG.add('--mt', inherits=True)
for n in ('--rt', '--idok', '--pidh', '--pidl', '--need-auth', '--need-body'):
    REG.add(n, inherits=True)
for f in 'ab':
    for n in ('m', 'l0', 'l1', 'l2'):
        REG.add('--r%s-%s' % (n, f), inherits=True)
for j in range(1, S + 1):
    REG.add('--rq%d' % j)
REG.add('--rpm', inherits=True)
REG.add('--rov')
for n in ('--rva', '--rka', '--rvb', '--rkb'):
    REG.add(n)


# ------------------------------------------------------------------------------------------------ automaton
def lit(s):
    return And(*[P('--c%d' % i).eq(ord(ch)) for i, ch in enumerate(s)])


def trans(M, c):
    """Next mode from mode M on byte c (a Pr each)."""
    dig, plus, slash, ques = c.rng(48, 57), c.eq(43), c.eq(47), c.eq(63)

    def leave(sl, qu):
        return [(slash, If([(mt.eq(1), sl)], 0)), (ques, If([(mt.eq(0), qu)], 0))]

    cases = [
        (M.eq(0) | M.ge(40), M),
        (c.eq(0), M),
        (M.eq(3), If([(dig, 3)] + leave(5, 43), 11)),
        (M.eq(1), If([(dig, 3), (plus, 2), (slash | ques, 0)], 11)),
        (M.eq(2), If([(dig, 3)] + leave(15, 53), 11)),
        (M.eq(11), If(leave(15, 53), 11)),
    ]
    for m0, ch in ((5, 'l'), (6, 'i'), (7, 'k'), (8, 'e')):
        cases.append((M.eq(m0) & c.eq(ord(ch)), m0 + 1))
        cases.append((M.eq(m0 + 10) & c.eq(ord(ch)), m0 + 11))
    cases.append((M.eq(9) & ques & mt.eq(1), 45))
    cases.append((M.eq(19) & ques & mt.eq(1), 55))
    return If(cases, 0)


def ind(M1, c):
    """1 when byte c (consumed leaving mode M1) was a digit of the id: mode 3 afterwards and not padding."""
    return (1 - Min(1, Max(P(M1) - 3, 3 - P(M1)))) * Min(1, P(c))


def horner(modes, cs):
    """Value of the digit run over positions (cs[i], modes[i] = mode after consuming cs[i]) and the digit count."""
    v, k = K(0), K(0)
    for m1, c in zip(modes, cs):
        i = ind(m1, c)
        v = v * (1 + 9 * i) + i * (P(c) - 48)
        k = k + i
    return v, k


def finals(mE, pidl_x, pidh_x, nonzero):
    """The six outputs from the final mode `mE` (Pr) and expressions for the id limbs."""
    rt = P('--rt')
    idrt = rt.eq(3) | rt.eq(5)
    return [
        ('--rt', If([(mE.ge(40), Mod(mE, 10)),
                     (mE.isin(2, 3, 11) & mt.eq(0), 3),
                     (mE.isin(9, 19) & mt.eq(1), 5)], 0)),
        ('--pidl', If([(idrt, pidl_x)], 0)),
        ('--pidh', If([(idrt, pidh_x)], 0)),
        ('--idok', If([(idrt & mE.isin(3, 9, 43, 45) & nonzero, 1), (idrt, 0)], 1)),
        ('--need-auth', If([(rt.eq(4) | rt.eq(5), 1)], 0)),
        ('--need-body', If([(rt.eq(4), 1)], 0)),
    ]


def first_decls():
    c = lambda i: P('--c%d' % i)
    rpm = P('--rpm')
    ds = [('--rpm', If([
        (lit('/feed') & (ln_.eq(5) | c(5).eq(63)) & mt.eq(0), 42),
        (lit('/posts') & c(6).eq(47) & mt.le(1), 1),
        (lit('/health') & (ln_.eq(7) | c(7).eq(63)) & mt.eq(0), 41),
        (lit('/posts') & (ln_.eq(6) | c(6).eq(63)) & mt.eq(1), 44),
    ], 0))]
    # mode after byte 7 (the first byte of the id) and its digit value, handed to the next chunk
    after7 = If([(c(7).rng(48, 57), 3), (c(7).eq(43), 2), (c(7).isin(0, 47, 63), 0)], 11)
    ds.append(('--rm-b', If([(rpm.eq(1), after7)], rpm)))
    ds.append(('--rl0-b', If([(rpm.eq(1) & c(7).rng(48, 57), c(7) - 48)], 0)))
    rt = P('--rt')
    ds.append(('--rt', If([(rpm.ge(40), Mod(rpm, 10))], 0)))
    ds.append(('--need-auth', If([(rt.eq(4) | rt.eq(5), 1)], 0)))
    ds.append(('--need-body', If([(rt.eq(4), 1)], 0)))
    return ds


def next_decls(fam_in, fam_out):
    ds = []
    c = lambda i: P('--c%d' % i)
    prev = '--rm-' + fam_in
    modes, cs = [], []
    for j in range(S):
        nxt = '--rq%d' % (j + 1)
        ds.append((nxt, trans(P(prev), c(j))))
        modes.append(nxt)
        cs.append('--c%d' % j)
        prev = nxt
    h = S // 2
    va, ka = horner(modes[:h], cs[:h])
    vb, kb = horner(modes[h:], cs[h:])
    ds += [('--rva', va), ('--rka', ka), ('--rvb', vb), ('--rkb', kb)]       # each half: <= 4 digits, exact as a property
    acc = (P('--rl2-' + fam_in) * 1000000000000 + P('--rl1-' + fam_in) * LIM + P('--rl0-' + fam_in)) * \
        Pow(10, P('--rka') + P('--rkb')) + P('--rva') * Pow(10, P('--rkb')) + P('--rvb')
    out = lambda n: '--r%s-%s' % (n, fam_out)
    ds.append((out('l0'), Mod(acc, LIM)))
    ds.append((out('l1'), Mod(Div(acc, LIM), LIM)))
    ds.append((out('l2'), Min(Div(acc, 1000000000000), 999999)))
    ds.append(('--rov', Min(1, Max(0, acc - 9223372036854775808 + 1))))
    M = P(prev)
    ov = P('--rov').eq(1)
    ds.append((out('m'), If([(ov & M.eq(3), 11), (ov & (M.rng(5, 9) | M.isin(43, 45)), M + 10)], M)))
    mE = P(out('m'))
    l0, l1, l2 = P(out('l0')), P(out('l1')), P(out('l2'))
    ds += finals(mE, l0, l2 * LIM + l1, l0.ne(0) | l1.ne(0) | l2.ne(0))
    return ds


class _Id(dict):
    def __missing__(self, k): return k


def decl_lists():
    """class -> [Decl]  (identity binding: logical names are the real property names)"""
    b = _Id()
    return {
        'Rf': [Decl(n, e, b) for n, e in first_decls()],
        'R1': [Decl(n, e, b) for n, e in next_decls('b', 'a')],
        'R0': [Decl(n, e, b) for n, e in next_decls('a', 'b')],
    }


def rules():
    # the first chunk writes family b, so the second chunk (R1) reads b and writes a, the third (R0) reads a, ...
    L = decl_lists()
    css = lambda cls: '.%s{%s}\n' % (cls, ''.join(d.css() for d in L[cls]))
    return css('Rf') + '@container style(--rpm: 1){' + css('R1') + css('R0') + '}\n'


def properties():
    return REG.listing()


# ------------------------------------------------------------------------------------------------ python simulation
OUT = ('--rt', '--idok', '--pidh', '--pidl', '--need-auth', '--need-body')


def simulate(path, method):
    """Run the declarations over a request target (bytes) the way the chained chunks would. method: 0 GET, 1 POST, 2 other."""
    lists = decl_lists()
    env = {n: 0 for n, *_ in REG.listing()}
    env['--mt'] = method
    env['--len'] = len(path)
    chunks = [path[i:i + S] for i in range(0, len(path), S)] or [b'']
    chunks += [b''] * (2 - len(chunks))
    for k, ch in enumerate(chunks):
        for i in range(S):
            env['--c%d' % i] = ch[i] if i < len(ch) else 0
        if k == 0:
            run_decls(lists['Rf'], env)
        elif env['--rpm'] == 1:
            run_decls(lists['R1' if k % 2 == 1 else 'R0'], env)
    return {n: env[n] for n in OUT}


if __name__ == '__main__':
    print(rules()[:2000])
