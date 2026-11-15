#!/usr/bin/env python3
"""B stage: request-body JSON parsing, mirroring Go's  json.NewDecoder(r).Decode(&struct{ Body any `json:"body"` })
followed by  body, _ := req.Body.(string); strings.TrimSpace; len(utf16.Encode([]rune(body))).

DOM: one element per `body_chunk` bytes, classes `B B0` / `B B1` alternating, inline --rid --n --off --len --c0.. (bytes; the
unused tail of the last chunk is 0). All rules sit inside  @container style(--need-body: 1) and style(--au: 3).

Outputs (inherited): --bj (0 malformed JSON, 1 valid but `body` missing / not a string / null document / empty input,
2 `body` is a string), --blen (UTF-16 length of the trimmed string), --bep (which `body` key occurrence wins).
Per-position event outputs (counter-reset of each B element, exact int32): a{j} and b{j} for j = 0..chunk-1:
    -1 = nothing; otherwise  ep * 2^21 + payload,  ep = epoch of the `body` key occurrence the string belongs to (the glue
    keeps the events with ep == bep, in order, and cuts the text to --blen UTF-16 units = the trimmed length), and
    payload = code point (slot b), or 0x110000 + n (slot a: n replacement characters U+FFFD for an aborted UTF-8 sequence /
    unpaired surrogate). Leading white space is dropped here; trailing white space is cut by --blen.

Semantics implemented (Go encoding/json scanner + decoder):
  * first non-space byte decides: end of input -> required; `null` (then anything) -> required; `{` -> object; else malformed
    (a top-level string / number / bool / array is an unmarshal type error, also malformed);
  * the first object is syntax-checked up to its closing brace (strict JSON: no trailing commas, escapes only
    \\" \\\\ \\/ \\b \\f \\n \\r \\t \\uXXXX, no raw control characters in strings, number grammar), bytes after it are ignored,
    truncation anywhere is malformed; nesting is supported up to 18 levels (deeper -> malformed);
  * keys of that object are matched against `body` case-insensitively AFTER unescaping (so \\u0062ody matches); the last
    occurrence wins (also over a non-string later value, e.g. {"body":"x","body":5} -> required);
  * the string is unescaped like Go (\\uD83D\\uDE00 pairs; unpaired surrogates -> U+FFFD; invalid UTF-8 -> U+FFFD),
    TrimSpace uses unicode.IsSpace, the length counts UTF-16 units.
Deviations: raw NUL bytes in the body are treated as padding (ignored); invalid UTF-8 sequences may yield a different number
of U+FFFD than Go in rare cases; numbers beyond float64 range (Go: unmarshal error) are accepted.
"""
from common import *

REG = Registry()
S = constants()['body_chunk']
MAXD = 262144   # a push is refused (-> malformed) at d >= MAXD: nesting depth <= 18 keeps d <= 999999

# ---- lexical states -----------------------------------------------------------------------------------------------
ERR, START, DONE, OBJ1, OBJK, KEYEND, KEYENDB, VAL, VALB, ARR1, AVA, AVO = range(12)
T1, T2, T3, F1, F2, F3, F4, N1, N2, N3 = range(12, 22)
NM, ND, NE, NS = 22, 23, 24, 25
NZ, NI, NF, NX = 26, 28, 30, 32            # + top (0 array, 1 object)
STR, ESC, UH4, UH3, UH2, UH1 = 34, 35, 36, 37, 38, 39
U2_1, U3_2, U3_1, U4_3, U4_2, U4_1 = 40, 41, 42, 43, 44, 45
HS, HSB, HU4, HU3, HU2, HU1 = 46, 47, 48, 49, 50, 51
STRINGISH = (STR, U2_1, U3_2, U3_1, U4_3, U4_2, U4_1, HS)   # states in which a '"' ends the string (possibly after an abort)

# per-position state properties (letter -> initial value of the family at the start of the body)
PROPS = {'s': START, 'd': 1, 'p': 0, 'h': 0, 'k': -1, 'e': 0, 'l': 0, 'w': 0}
OUTS = ('--bj', '--blen', '--bep')

for f in 'ab':
    for l, init in PROPS.items():
        REG.add('--b%s-%s' % (l, f), inherits=True, initial=init)
for j in range(1, S):
    for l in PROPS:
        REG.add('--b%s%d' % (l, j))
for j in range(S):
    for l in ('ea', 'ec', 'eb', 'x', 'ws', 'dg', 'hx', 'ct', 'sm', 'sg'):
        REG.add('--b%s%d' % (l, j))
    REG.add('--c%d' % j)
for n in ('--n', '--off', '--len'):
    REG.add(n)
REG.add('--bj', inherits=True, initial=1)       # no B element = empty input = "body is required"
REG.add('--blen', inherits=True)
REG.add('--bep', inherits=True)


def nm(l, j, fin, fout):
    if j == 0: return '--b%s-%s' % (l, fin)
    if j == S: return '--b%s-%s' % (l, fout)
    return '--b%s%d' % (l, j)


def P(n): return Pr(n)


UNICODE_SPACE = (9, 10, 11, 12, 13, 32, 133, 160, 5760, 8192, 8193, 8194, 8195, 8196, 8197, 8198, 8199, 8200, 8201, 8202,
                 8232, 8233, 8239, 8287, 12288)


def is_space(eb):
    """Condition: the code point in prop eb is white space for strings.TrimSpace (unicode.IsSpace)."""
    return Or(eb.rng(9, 13), eb.eq(32), eb.eq(133), eb.eq(160), eb.eq(5760), eb.rng(8192, 8202), eb.eq(8232), eb.eq(8233),
              eb.eq(8239), eb.eq(8287), eb.eq(12288))


def decls_for_position(j, fin, fout):
    """[(lhs, expr)] for one byte position: state at j -> state at j+1, the events at j and the outputs of j."""
    cur = {l: P(nm(l, j, fin, fout)) for l in PROPS}
    nxt = {l: nm(l, j + 1, fin, fout) for l in PROPS}
    s, d, p, h, k, e, ln, lw = (cur[x] for x in 'sdphkelw')
    c = P('--c%d' % j)
    ea, ec, eb = P('--bea%d' % j), P('--bec%d' % j), P('--beb%d' % j)
    pn = P(nxt['p'])

    # byte classes, one 0/1 property each (shared by many conditions: keeps the declaration text small)
    ws, dig, hexd, cont, simple_ = (P('--b%s%d' % (n, j)).eq(1) for n in ('ws', 'dg', 'hx', 'ct', 'sm'))
    dig19 = dig & ~c.eq(48)
    sg = P('--bsg%d' % j)
    hi_sg, lo_sg = sg.eq(1), sg.eq(2)
    t = Mod(d, 2)
    AVt = 10 + t
    VEND = If([(d.eq(1), DONE)], AVt)
    POPT = If([(d.eq(3), DONE)], 10 + Mod(d // 2, 2))
    ea = P('--bea%d' % j)
    strend = If([(k.rng(0, 9), If([(k.eq(4) & ea.eq(0), KEYENDB)], KEYEND))], AVt)

    sx = P('--bx%d' % j)       # what a byte c does in the plain string state (also used when a pending sequence aborts)
    sx_e = If([(c.eq(34), strend), (c.eq(92), ESC), (c.lt(32), ERR), (c.rng(194, 223), U2_1), (c.rng(224, 239), U3_2),
               (c.rng(240, 244), U4_3)], STR)
    base = sx
    vs = If([(c.eq(34), STR),
             (c.eq(123), If([(d.lt(MAXD), OBJ1)], ERR)),
             (c.eq(91), If([(d.lt(MAXD), ARR1)], ERR)),
             (c.eq(116), T1), (c.eq(102), F1), (c.eq(110), N1), (c.eq(45), NM), (c.eq(48), NZ + t), (dig19, NI + t)], ERR)
    ee = c.isin(69, 101)
    term = ws | c.isin(44, 93, 125)
    numA = s.isin(NZ, NI, NF, NX)
    numO = s.isin(NZ + 1, NI + 1, NF + 1, NX + 1)

    def lit(nxt_state, ch):
        return If([(c.eq(ord(ch)), nxt_state)], ERR)

    def hexnext(nxt_state):
        return If([(hexd, nxt_state)], ERR)

    srows = [
        (s.eq(STR), base),
        (s.isin(DONE, ERR), s),
        (ws & s.isin(START, OBJ1, OBJK, KEYEND, KEYENDB, VAL, VALB, ARR1, AVA, AVO), s),
        ((s.eq(AVO) | (numO & term)), If([(ws, AVO), (c.eq(44), OBJK), (c.eq(125), POPT)], ERR)),
        ((s.eq(AVA) | (numA & term)), If([(ws, AVA), (c.eq(44), VAL), (c.eq(93), POPT)], ERR)),
        (s.isin(VAL, VALB, ARR1), If([(s.eq(ARR1) & c.eq(93), POPT)], vs)),
        (s.eq(OBJK), If([(c.eq(34), STR)], ERR)),
        (s.eq(KEYEND), If([(c.eq(58), VAL)], ERR)),
        (s.eq(OBJ1), If([(c.eq(34), STR), (c.eq(125), POPT)], ERR)),
        (s.eq(KEYENDB), If([(c.eq(58), VALB)], ERR)),
        (s.eq(ESC), If([(c.eq(117), UH4), (simple_, STR)], ERR)),
        (s.isin(UH4, UH3, UH2), If([(hexd, s + 1)], ERR)),
        (s.eq(UH1), If([(hexd, If([(hi_sg, HS)], STR))], ERR)),
        (s.eq(U2_1), If([(cont, STR)], base)),
        (s.eq(U3_2), If([(cont & pn.ge(32) & ~pn.rng(864, 895), U3_1)], base)),
        (s.eq(U3_1), If([(cont, STR)], base)),
        (s.eq(U4_3), If([(cont & pn.rng(16, 271), U4_2)], base)),
        (s.eq(U4_2), If([(cont, U4_1)], base)),
        (s.eq(U4_1), If([(cont, STR)], base)),
        (s.eq(HS), If([(c.eq(92), HSB)], base)),
        (s.eq(HSB), If([(c.eq(117), HU4), (simple_, STR)], ERR)),
        (s.isin(HU4, HU3, HU2), If([(hexd, s + 1)], ERR)),
        (s.eq(HU1), If([(hexd, If([(hi_sg, HS)], STR))], ERR)),
        (s.eq(T1), lit(T2, 'r')), (s.eq(T2), lit(T3, 'u')), (s.eq(T3), If([(c.eq(ord('e')), VEND)], ERR)),
        (s.eq(F1), lit(F2, 'a')), (s.eq(F2), lit(F3, 'l')), (s.eq(F3), lit(F4, 's')), (s.eq(F4), If([(c.eq(ord('e')), VEND)], ERR)),
        (s.eq(N1), lit(N2, 'u')), (s.eq(N2), lit(N3, 'l')), (s.eq(N3), If([(c.eq(ord('l')), VEND)], ERR)),
        (s.eq(NM), If([(c.eq(48), NZ + t), (dig19, NI + t)], ERR)),
        (s.eq(ND), If([(dig, NF + t)], ERR)),
        (s.eq(NE), If([(c.isin(43, 45), NS), (dig, NX + t)], ERR)),
        (s.eq(NS), If([(dig, NX + t)], ERR)),
        (s.isin(NZ, NZ + 1), If([(c.eq(46), ND), (ee, NE)], ERR)),
        (s.isin(NI, NI + 1), If([(dig, s), (c.eq(46), ND), (ee, NE)], ERR)),
        (s.isin(NF, NF + 1), If([(dig, s), (ee, NE)], ERR)),
        (s.isin(NX, NX + 1), If([(dig, s)], ERR)),
        (s.eq(START), If([(ws | c.eq(0), START), (c.eq(123), OBJ1), (c.eq(110), N1)], ERR)),
    ]
    sn = If(srows, ERR)

    # ---- accumulator (UTF-8 / \u digits) ----
    hv = If([(dig, c - 48), (c.rng(65, 70), c - 55)], c - 87)
    leadp = If([(c.rng(194, 223), c - 192), (c.rng(224, 239), c - 224), (c.rng(240, 244), c - 240)], 0)
    pexpr = If([
        (s.eq(STR), leadp),
        (s.rng(U2_1, U4_1), If([(cont, p * 64 + (c - 128))], leadp)),
        (s.isin(UH4, UH3, UH2, UH1, HU4, HU3, HU2, HU1), p * 16 + hv),
        (s.eq(HS), leadp),
    ], 0)

    # ---- events ----
    simple = simple_
    esc_val = If([(c.eq(98), 8), (c.eq(102), 12), (c.eq(110), 10), (c.eq(114), 13), (c.eq(116), 9)], c)
    plain = c.rng(32, 127) & ~c.eq(34) & ~c.eq(92)
    stray = cont | c.isin(192, 193) | c.ge(245)
    base_ec = If([(plain, 0), (stray, 0)], -1)
    base_eb = If([(plain, c), (stray, 65533)], 0)
    cp4 = p * 64 + (c - 128)           # exact only inline (up to 0x10FFFF): never read through a property
    pair = 65536 + (h - 55296) * 1024 + (pn - 56320)
    ab_u3 = ~(cont & pn.ge(32) & ~pn.rng(864, 895))
    ab_u4 = ~(cont & pn.rng(16, 271))
    ea_cases = [
        (s.eq(U2_1) & ~cont, 1), (s.eq(U3_2) & ab_u3, 1), (s.eq(U3_1) & ~cont, 2),
        (s.eq(U4_3) & ab_u4, 1), (s.eq(U4_2) & ~cont, 2), (s.eq(U4_1) & ~cont, 3),
        (s.eq(HS) & ~c.eq(92), 1), (s.eq(HSB) & simple, 1),
        (s.eq(HU1) & hexd & ~lo_sg, 1),
    ]
    ec_cases = [
        (s.eq(STR), base_ec),
        (s.eq(ESC) & simple, 0),
        (s.eq(HSB) & simple, 0),
        (s.eq(UH1) & hexd & ~hi_sg, 0),
        (s.eq(HU1) & hexd & lo_sg, pair // 65536),
        (s.eq(HU1) & hexd & ~hi_sg, 0),
        (s.eq(HU1) & hexd, -1),
        (ea.gt(0), base_ec),                      # an aborted UTF-8 sequence / pending surrogate: c is reprocessed as in STR
        (s.eq(U2_1) | s.eq(U3_1), 0),
        (s.eq(U4_1), cp4 // 65536),
    ]
    eb_cases = [
        (s.eq(STR), base_eb),
        (s.eq(ESC) & simple, esc_val),
        (s.eq(HSB) & simple, esc_val),
        (s.eq(UH1) & hexd & lo_sg, 65533),
        (s.eq(UH1) & hexd & ~hi_sg, pn),
        (s.eq(HU1) & hexd & lo_sg, pair % 65536),
        (s.eq(HU1) & hexd & ~hi_sg, pn),
        (s.eq(HU1) & hexd, 0),
        (ea.gt(0), base_eb),
        (s.eq(U2_1) | s.eq(U3_1), pn),
        (s.eq(U4_1), cp4 % 65536),
    ]
    ea_e = If(ea_cases, 0)
    ec_e = If(ec_cases, -1)
    eb_e = If(eb_cases, 0)

    # ---- the rest of the state ----
    hn = If([((s.eq(UH1) | s.eq(HU1)) & hexd & hi_sg, pn)], h)
    dn = If([(c.isin(123, 91, 125, 93),
              If([(c.eq(123) & s.isin(START, VAL, VALB, ARR1), d * 2 + 1),
                  (c.eq(91) & s.isin(VAL, VALB, ARR1), d * 2),
                  (c.eq(125) & s.isin(OBJ1, AVO, NZ + 1, NI + 1, NF + 1, NX + 1), d // 2),
                  (c.eq(93) & s.isin(ARR1, AVA, NZ, NI, NF, NX), d // 2)], d))], d)
    letter = (k.eq(0) & (eb.eq(98) | eb.eq(66))) | (k.eq(1) & (eb.eq(111) | eb.eq(79))) | \
        (k.eq(2) & (eb.eq(100) | eb.eq(68))) | (k.eq(3) & (eb.eq(121) | eb.eq(89)))
    kn = If([
        (c.eq(34) & s.isin(OBJ1, OBJK), If([(d.eq(3), 0)], 9)),
        (c.eq(34) & s.eq(VALB), 10),
        (c.eq(34) & s.isin(VAL, ARR1), -1),
        (k.rng(0, 9) & ea.gt(0), 9),
        (k.rng(0, 3) & ec.eq(0) & letter, k + 1),
        (k.rng(0, 9) & ec.ge(0), 9),
    ], k)
    keyclose = c.eq(34) & k.eq(4) & ea.eq(0) & s.isin(*STRINGISH)
    en = If([
        (s.eq(VALB) & c.eq(34), e - Mod(e, 2) + 1),
        (s.eq(VALB) & ~ws & ~c.eq(0), e - Mod(e, 2)),
        (keyclose, Min(e + 2 - Mod(e, 2), 2047)),
    ], e)
    bodystart = s.eq(VALB) & c.eq(34)
    drop = lw.eq(0) & ea.eq(0) & ec.eq(0) & is_space(eb)
    ub = If([(ec.lt(0), 0), (ec.ge(1), 2), (drop, 0)], 1)
    lnn_e = If([(bodystart, 0), (k.eq(10), ln + ea + ub)], ln)
    nonws_main = ec.ge(1) | (ec.eq(0) & ~is_space(eb))
    lwn_e = If([(bodystart, 0), (k.eq(10) & nonws_main, P(nxt['l'])), (k.eq(10) & ea.gt(0), ln + ea)], lw)

    ds = [
        ('--bws%d' % j, If([(c.isin(32, 9, 10, 13), 1)], 0)),
        ('--bdg%d' % j, If([(c.rng(48, 57), 1)], 0)),
        ('--bhx%d' % j, If([(c.rng(48, 57) | c.rng(65, 70) | c.rng(97, 102), 1)], 0)),
        ('--bct%d' % j, If([(c.rng(128, 191), 1)], 0)),
        ('--bsm%d' % j, If([(c.isin(34, 92, 47, 98, 102, 110, 114, 116), 1)], 0)),
        (nxt['p'], pexpr),
        ('--bsg%d' % j, If([(pn.rng(55296, 56319), 1), (pn.rng(56320, 57343), 2)], 0)),
        ('--bea%d' % j, ea_e), ('--bec%d' % j, ec_e), ('--beb%d' % j, eb_e),
        ('--bx%d' % j, sx_e),
        (nxt['s'], sn),
        (nxt['d'], dn), (nxt['h'], hn), (nxt['k'], kn), (nxt['e'], en),
        (nxt['l'], lnn_e), (nxt['w'], lwn_e),
    ]
    # output counters of this position
    ep = (e // 2) * 2097152
    out_a = If([(k.eq(10) & ea.gt(0), ep + 1114112 + ea)], -1)
    out_b = If([(k.eq(10) & ec.ge(0) & ~drop, ep + ec * 65536 + eb)], -1)
    return ds, (('a%d' % j, out_a), ('b%d' % j, out_b))


def chunk_decls(fin, fout):
    ds, outs = [], []
    for j in range(S):
        d, o = decls_for_position(j, fin, fout)
        ds += d
        outs += o
    s_end, e_end, w_end = (P(nm(l, S, fin, fout)) for l in 'sew')
    ds.append(('--bj', If([(s_end.eq(DONE), 1 + Mod(e_end, 2)), (s_end.eq(START), 1)], 0)))
    ds.append(('--blen', w_end + 0))
    ds.append(('--bep', e_end // 2))
    return ds, outs


class _Id(dict):
    def __missing__(self, k): return k


GATE = '@container style(--need-body: 1) and style(--au: 3)'


def decl_lists():
    b = _Id()
    r = {}
    for cls, (fin, fout) in (('B0', ('a', 'b')), ('B1', ('b', 'a'))):
        ds, outs = chunk_decls(fin, fout)
        r[cls] = ([Decl(n, e, b) for n, e in ds], outs)
    return r


def rules():
    b = _Id()
    out = []
    for cls, (ds, outs) in decl_lists().items():
        body = ''.join(d.css() for d in ds)
        body += 'counter-reset:' + ' '.join('%s %s' % (n, render(e, b)) for n, e in outs) + ';'
        out.append('.%s{%s}' % (cls, body))
    return GATE + '{' + '\n'.join(out) + '}\n'


def properties():
    return REG.listing()


def weak_properties():
    return [('--need-body', '<integer>', True, 0), ('--au', '<integer>', True, 0)]


# ------------------------------------------------------------------------------------------------ python simulation
def simulate(body):
    """Run the declarations over the body bytes. -> dict(bj, blen, bep, events=[(slot, value)])"""
    lists = decl_lists()
    env = {n: 0 for n, *_ in REG.listing()}
    for (l, init) in PROPS.items():
        for f in 'ab':
            env['--b%s-%s' % (l, f)] = init
    env['--bj'] = 1
    chunks = [body[i:i + S] for i in range(0, len(body), S)]
    events = []
    for kx, ch in enumerate(chunks):
        for i in range(S):
            env['--c%d' % i] = ch[i] if i < len(ch) else 0
        ds, outs = lists['B0' if kx % 2 == 0 else 'B1']
        run_decls(ds, env)
        for name, expr in outs:
            v = evaluate(expr, _Id2(env))
            if v >= 0:
                events.append((name[0], v))
    if not chunks:
        pass
    return {'bj': env['--bj'], 'blen': env['--blen'], 'bep': env['--bep'], 'events': events}


class _Id2:
    def __init__(self, e): self.e = e
    def __getitem__(self, k): return self.e[k]


def text_of(events, bep, blen):
    """What the glue does: events of the winning epoch -> string -> cut to blen UTF-16 units."""
    cps = []
    for slot, v in events:
        if v >> 21 != bep:
            continue
        x = v & 0x1FFFFF
        if x > 0x10FFFF:
            cps.extend([0xFFFD] * (x - 0x110000))
        else:
            cps.append(x)
    u16 = []
    for cp in cps:
        if cp >= 0x10000:
            cp -= 0x10000
            u16.extend([0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF)])
        else:
            u16.append(cp)
    return u16[:blen]


if __name__ == '__main__':
    print('body css size', len(rules()))
