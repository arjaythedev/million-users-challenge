#!/usr/bin/env python3
"""H stage ("frame pass"): HTTP request framing over the raw head bytes, mirroring  httparse 1.10.1  +  src/http.rs::parse.

TWO PHASES.  Phase 1 (always): `Hs` + `Z` only.  Phase 2 (only when Z.slow = 1): the same plus the chunk elements.

    <b class='Hs' style='--rid;--len:{window length}' d='{window}' e='{classes}' g='{digit-folded}'>        head summary (phase 1)
      <b class='H H0' dc='{classes of the chunk}' style='--rid;--off:0;--h0..--h15'>                      phase 2: one per 16 bytes,
        <b class='H H1' dc=.. style='--rid;--off:16;--h0..'> ...      classes alternate H0 / H1, the LAST one also has class Hl;
          <b class='Z Z{n%2}' id='z{rid}' style='--rid;--off:{16n}'>   missing --h{i} in the last chunk = padding; n = chunk count
    (phase 1: no chunks, class 'Z Z0')

* window = the first min(buffer length, HEAD_MAX = 2048) bytes of the connection buffer. Once the blank line was seen the rest is
  ignored (body, next pipelined request).
* d = window, each byte as `&#N;` unless printable ASCII other than & ' " < > \\ (backslash: the fragment travels inside a JSON string).  e = '^' + class per byte + '$' with classes
  t (token char: alnum - . _ ~ ! # $ % & ' * + ^ ` |), u (other printable except ':'), ':', s (SP), r (CR), n (LF), ! (anything else:
  control, HT, DEL, NUL, >= 0x80).  g = first 64 bytes of d with every digit written as 9, except a digit next to a '.' (the version digits stay).  dc = the class string of the chunk (no
  sentinels).  These are plain per-byte table lookups, no state.
* Tier S: attribute selectors on Hs certify light GETs of /feed, /health, /posts/<digits> (see tier_s_rules) -> Z.slow = 0 and every
  output is final after phase 1 (~70 us per request instead of ~2.5 ms).  Otherwise Z.slow = 1 and the front adds the chunks
  (tier E: a per-byte automaton, exact for everything else, ~20 ms for a 300 B head).
* In tier E the events of a chunk are decoded by its child (next chunk or Z), gated by container queries on flags the parent wrote.

Z outputs (counter-reset on .Z, exact int32), meaningful when ok = 1:
    ok     1 = a complete, valid head was found (httparse Complete and no application-level rejection)
    bad    1 = reject (close the connection): httparse error, Transfer-Encoding, bad / too large Content-Length (> 49152),
           more than 64 headers.  ok = 0 and bad = 0: head incomplete (wait for more bytes; the front closes if the window is
           already HEAD_MAX long).
    hl     head length = offset just after the blank line (== httparse Complete(n))
    cl     Content-Length (last header wins; 0 when absent)
    used   hl + cl
    close  1 = close after the response (HTTP/1.0 default, `Connection: close` / `keep-alive`)
    cont   1 = an `Expect: 100-continue` header is present
    mt     0 GET, 1 POST, 2 other (exact, case-sensitive method token)
    ps pe  request-target = window[ps, pe)
    as ae  value of the FIRST Authorization header, ends trimmed (as == ae: empty value; then both are unspecified but equal);
           0 / 0 when absent
    slow   1 = tier S did not certify the head; with phase 1 only, the other outputs are void: send the chunks (phase 2)

Deviations from httparse (each one covered by a case in the local differential test corpus):
  * request-target bytes >= 0x80 are accepted without the UTF-8 check; method tokens may contain any printable non-blank byte;
  * Content-Length whose value has non-ASCII bytes (str::trim would strip Unicode blanks) is rejected;
  * an Authorization value with more than 255 trailing blanks is rejected;
  * heads longer than the window (HEAD_MAX) are never complete;
  * bad is reported as soon as the offending header line is seen (Transfer-Encoding, Content-Length), httparse would still wait;
  * `Connection` / `Expect` (close, keep-alive, 100-continue) are recognised by attribute selectors on `d`: blanks around the value
    are limited to 0..2 spaces or one tab on each side, and when several such headers occur the order is not considered
    (close wins over keep-alive, any Expect: 100-continue wins);
  * tier S: heads of >= 208 bytes (could have > 64 headers) never use it, nor do request targets with a query string, a leading '+'
    in the id, more than 19 id digits, and so on (they take tier E: slow = 1, never wrong).
"""
from common import *

REG = Registry()
S = constants()['raw_chunk']
assert S == 16, 'raw_chunk must be 16'

# ------------------------------------------------------------------------------------------------ modes
L0, L0CR = 10, 11                     # start (leading blank lines are skipped) / CR of a leading blank line
G1, G2, G3, P1, P2, P3, P4, MO = 20, 21, 22, 23, 24, 25, 26, 27   # method prefix states, MO = other token
U0 = 30                               # +mt: after the method blank, the request-target must start
UR = 40                               # +10*mt: inside the request-target
VH = 70                               # VH..VH+6 expect H T T P / 1 .   VH+7 expects the digit 0/1
VE0, VE1, VC0, VC1 = 80, 81, 82, 83   # version complete / CR after it
HS, HCR, HSP = 100, 101, 102          # header line start / CR at a line start / line start after padding (not counted)
ND = 110                              # header name that is not one of the two interesting ones
AC = 200                              # +kind (0 other, 1 content-length, 3 authorization): after the colon, skipping blanks
VO, LCR0 = 210, 211                   # value of an ordinary header / CR after it
CLP = 220                             # content-length: after '+'
VA = 290                              # authorization value, last byte not blank
VAW = 1000                            # +tw (1..255): authorization value, tw trailing blanks so far
DONE, ERR = 900, 901
NMC, NMA = 700000, 700100             # +i: the first i bytes of `content-length` / `authorization` matched exactly (case-insensitively)
CLD, CLT, LCR1 = 100000, 200000, 300000    # +value (<= 99999): content-length digits / trailing blanks / CR
LCRA, LCRE = 400000, 410000                # +position: CR after an authorization value (ae) / after an empty one
K4 = 500000                                # +position of the first value byte of an authorization header (acts as VA)
K1 = 510000                                # +cl: content-length line finished        (acts as HS)
K5 = 620000                                # +ae: authorization line finished           (acts as HS)
K6 = 630000                                # +position: authorization line finished with an empty value (acts as HS)
HSMAX = 649999
BIG = 99999
MAX_CL = 49152
HEAD_BIG = constants()['frame_big']        # a head with 65 header lines needs >= 208 bytes (also: the front goes straight to phase 2 from here)
HEAD_MAX = constants()['head_max']         # the window
TARGETS = ((NMC, 'content-length', 1), (NMA, 'authorization', 3))


def packed(name):
    """name as two base-128 words (7 characters each): exact in a double."""
    ws = [0, 0]
    for k, ch in enumerate(name):
        ws[k // 7] += ord(ch) << (7 * (k % 7))
    return ws


# tchar bit masks (c = 33..79 -> bit c-33 of TM1, c = 80..126 -> bit c-80 of TM2)
_tch = lambda c: 97 <= c <= 122 or 65 <= c <= 90 or c == 45 or 48 <= c <= 57 or c in (33, 35, 36, 37, 38, 39, 42, 43, 46, 94, 95, 96, 124, 126)
TM1 = sum(1 << (c - 33) for c in range(33, 80) if _tch(c))
TM2 = sum(1 << (c - 80) for c in range(80, 127) if _tch(c))


def tchar_bit(c):
    """1 when c (33..126) is a tchar. Arithmetic only (no style() query); mask * 2^-(c-33) is exact in a double."""
    s = Min(1, Max(0, c - 79))
    return Mod(Div((TM1 + (TM2 - TM1) * s) * Pow(2, 33 + 47 * s - c), 1), 2)


def lower(c):
    """ASCII lower-case (for letters; other characters only need to map consistently)."""
    return c + 32 * (1 - Mod(Div(c, 32), 2))


# ------------------------------------------------------------------------------------------------ selectors on `d`
def _ws(*xs):
    return list(xs)


BLANKS = ['', ' ', '  ', '\t']


def _header_value_patterns(name, value):
    out = []
    for w1 in BLANKS:
        for w2 in BLANKS:
            for eol in ('\r', '\n'):
                out.append('\n%s:%s%s%s%s' % (name, w1, value, w2, eol))
    return out


# flag -> (presence pattern, [value patterns] or None);  all ASCII case-insensitive (`i`), matched anywhere in `d`
SELECTORS = [
    ('--shcl', '\ncontent-length:', None),
    ('--shau', '\nauthorization:', None),
    ('--ste', '\ntransfer-encoding:', None),
    ('--scc', '\nconnection:', _header_value_patterns('connection', 'close')),
    ('--sck', '\nconnection:', _header_value_patterns('connection', 'keep-alive')),
    ('--sex', '\nexpect:', _header_value_patterns('expect', '100-continue')),
]


def cssstr(s):
    out = []
    for ch in s:
        if ch in '\\"':
            out.append('\\' + ch)
        elif ord(ch) < 32:
            out.append('\\%06x' % ord(ch))
        else:
            out.append(ch)
    return ''.join(out)


def selector_rules():
    out = []
    for flag, pres, vals in SELECTORS:
        decl = '%s:1;' % flag + ('--shint:1;' if flag in ('--shcl', '--shau') else '')
        if vals is None:
            out.append('.Hs[d*="%s" i]{%s}\n' % (cssstr(pres), decl))
        else:
            out.append('.Hs[d*="%s" i]:is(%s){%s}\n' % (cssstr(pres), ','.join('[d*="%s" i]' % cssstr(v) for v in vals), decl))
    return ''.join(out)


def d_string(window):
    """The `d` attribute text as the HTML parser sees it (python reference for the simulation)."""
    return ''.join('�' if b == 0 else chr(b) for b in window)


def sim_flags(window):
    d = d_string(window).lower()
    f = {}
    for flag, pres, vals in SELECTORS:
        hit = pres in d and (vals is None or any(v.lower() in d for v in vals))
        f[flag] = 1 if hit else 0
    f['--shint'] = f['--shcl'] | f['--shau']
    return f


# ------------------------------------------------------------------------------------------------ tier S: whole-head certificate
# Light GETs of the three known routes are framed by attribute selectors alone (no chunk elements at all, ~50 us instead of ms):
#   e = '^' + class string + '$'  (tchar t, other printable u, ':' , SP s, CR r, LF n, everything else '!')
#   g = first 64 bytes of the window with every digit written as 9 (the route /posts/<id> has a variable length)
# The certificate (`--scert`) says: the head ends with exactly one blank line at the very end of the window (so hl = len), every line
# ends with CRLF, every line after the first is `name: ...` with a token name of <= NAME_K characters, no control / non-ASCII bytes.
# Together with a route match (`--sroute`: GET /feed, /health, /posts/<digits>, HTTP/1.1 or 1.0) and no interesting header
# (Content-Length, Authorization, Transfer-Encoding) the answer is complete. Anything else: slow = 1 and the front sends the chunks.
NAME_K = 40


def class_string(window):
    out = ['^']
    for c in window:
        out.append('t' if _tch(c) else 'u' if 33 <= c <= 126 and c != 58 else ':' if c == 58 else 's' if c == 32 else 'r' if c == 13
                   else 'n' if c == 10 else '!')
    out.append('$')
    return ''.join(out)


def fold_string(window):
    w = window[:64]
    out = []
    for i, b in enumerate(w):
        keep = (i > 0 and w[i - 1] == 46) or (i + 1 < len(w) and w[i + 1] == 46)
        out.append('9' if 48 <= b <= 57 and not keep else '\ufffd' if b == 0 else chr(b))
    return ''.join(out)


def forbidden_patterns():
    p = ['!', 'tn', 'un', ':n', 'sn', '^n', 'nn']                       # LF only after CR
    p += ['r' + x for x in 'tu:sr'] + ['r$']                              # CR only before LF
    p += ['n' + x for x in 'u:s']                                          # a line starts with a name (or is the blank line)
    p += ['rnrn' + x for x in 'tu:srn']                                    # no blank line before the end
    for k in range(1, NAME_K + 1):
        p += ['n' + 't' * k + x for x in 'usrn']                           # name is token chars followed by ':'
    p.append('n' + 't' * (NAME_K + 1))                                     # longer names: not certified
    return p


ROUTES = []      # (g or d prefix attr, prefix text, ps, pe, v0)
for ver, v0 in (('1.1', 0), ('1.0', 1)):
    ROUTES.append(('d', 'GET /feed HTTP/%s\r\n' % ver, 4, 9, v0))
    ROUTES.append(('d', 'GET /health HTTP/%s\r\n' % ver, 4, 11, v0))
POSTS_K = 19


def tier_s_rules():
    out = ['.Hs[e$="rnrn$"]%s{--scert:1}\n' % ''.join(':not([e*="%s"])' % p for p in forbidden_patterns())]
    for attr, pre, ps, pe, v0 in ROUTES:
        out.append('.Hs[%s^="%s"]{--sroute:1;--sps:%d;--spe:%d;--sv0:%d}\n' % (attr, cssstr(pre), ps, pe, v0))
    for ver, v0 in (('1.1', 0), ('1.0', 1)):
        for k in range(1, POSTS_K + 1):
            out.append('.Hs[g^="%s"]{--sroute:1;--sps:4;--spe:%d;--sv0:%d}\n'
                       % (cssstr('GET /posts/' + '9' * k + ' HTTP/%s\r\n' % ver), 11 + k, v0))
    return ''.join(out)


def sim_tier_s(window):
    """python mirror of tier_s_rules(): dict of the S registers."""
    e, g, d = class_string(window), fold_string(window), d_string(window)
    f = {'--scert': 0, '--sroute': 0, '--sps': 0, '--spe': 0, '--sv0': 0}
    if e.endswith('rnrn$') and not any(p in e for p in forbidden_patterns()):
        f['--scert'] = 1
    for attr, pre, ps, pe, v0 in ROUTES:
        if (d if attr == 'd' else g).startswith(pre):
            f.update({'--sroute': 1, '--sps': ps, '--spe': pe, '--sv0': v0})
    for ver, v0 in (('1.1', 0), ('1.0', 1)):
        for k in range(1, POSTS_K + 1):
            if g.startswith('GET /posts/' + '9' * k + ' HTTP/%s\r\n' % ver):
                f.update({'--sroute': 1, '--sps': 4, '--spe': 11 + k, '--sv0': v0})
    return f


# ------------------------------------------------------------------------------------------------ registrations
# q families: chunk of parity 'a' (H0) writes --ha1..--ha16, parity 'b' (H1) writes --hb1..--hb16; both inherited, so the CHILD
# (next chunk, or Z) can decode the events of its parent.
for i in range(S):
    REG.add('--h%d' % i, initial=256)
for n in ('--off', '--len'):
    REG.add(n)
for fam in 'ab':
    for j in range(1, S + 1):
        REG.add('--h%s%d' % (fam, j), inherits=True, initial=L0 if j == S else 0)
REG.add('--hmf', inherits=True, initial=L0)
for n in ('--hvd', '--hbig', '--hps', '--hpe', '--hmt', '--hv0', '--hhl', '--hcl', '--has', '--hae', '--hca', '--hcb', '--hhcf',
          '--shcl', '--shau', '--shint', '--ste', '--scc', '--sck', '--sex', '--scert', '--sroute', '--sps', '--spe', '--sv0', '--hlen'):
    REG.add(n, inherits=True)
for fam in 'ab':
    for n in ('hact', 'hev', 'hea', 'heb', 'hec', 'hed', 'hmk'):
        REG.add('--%s%s' % (n, fam), inherits=True)
REG.add('--sok')
for n in ('--he1', '--he4', '--he5', '--he6', '--he4v', '--he6w', '--he56'):
    REG.add(n)

off = Pr('--off')


def Q(fam, k):
    return Pr('--h%s%d' % (fam, k))


# ------------------------------------------------------------------------------------------------ the automaton
def trans(m, c, j, rl, sp, last=False):
    """Mode after byte `c` (a Pr) at chunk position j from mode `m` (a Pr).
    rl: also the request-line modes.  sp: also the Content-Length / Authorization machinery (only when such a header exists)."""
    P = off + j
    PAD = c.eq(256)
    CR, LF, SP, HT, COL = c.eq(13), c.eq(10), c.eq(32), c.eq(9), c.eq(58)
    OWS = SP | HT
    VB = c.rng(32, 126) | HT | c.rng(128, 255)           # header value byte (httparse)
    NOWS = c.rng(33, 126) | c.rng(128, 255)
    URIB = c.rng(33, 126) | c.rng(128, 255)
    DIG = c.rng(48, 57)
    PR = c.rng(33, 126)
    TB = tchar_bit(c)

    def D():          # no rule matched: an error (in the last chunk, which may be padded, padding keeps the mode)
        return If([(PAD, m)], ERR) if last else K(ERR)

    cases = []

    def add(mc, x):
        cases.append((mc, x))

    add(m.eq(VO), If([(VB, VO), (CR, LCR0), (LF, HS)], D()))
    add(m.eq(ND), If([(COL, AC), (PR, ERR + (ND - ERR) * TB)], D()))
    if rl:
        add(m.rng(UR, UR + 20), If([(URIB, m), (SP, VH)], D()))
    add(m.rng(DONE, ERR), m)
    add(m.eq(AC), If([(OWS, AC), (VB, VO), (CR, LCR0), (LF, HS)], D()))
    add(m.eq(LCR0), If([(LF, HS)], D()))
    # ---- line start: plain HS (HSP after padding) or any marker that acts as HS
    hsc = [(CR, HCR), (LF, DONE)]
    if sp:
        hsc.append((c.eq(67) | c.eq(99), NMC + 1))
        hsc.append((c.eq(65) | c.eq(97), NMA + 1))
    hsc.append((PR, ERR + (ND - ERR) * TB))
    add(m.eq(HS) | m.eq(HSP) | m.rng(K1, HSMAX), If(hsc, If([(PAD, HSP)], ERR) if last else K(ERR)))
    add(m.eq(HCR), If([(LF, DONE)], D()))
    if sp:
        # exact match of the two interesting names, byte by byte: state NMx + i = i bytes matched; the expected byte is taken out of
        # the packed constant by arithmetic and compared with the lower-cased input (a mismatch makes it an ordinary name)
        for base, name, kind in TARGETS:
            w0, w1 = packed(name)
            n = len(name)
            i = m - base
            s7 = Min(1, Max(0, i - 6))
            exp = Mod(Div((w0 + (w1 - w0) * s7) * Pow(2, 0 - 7 * (i - 7 * s7)), 1), 128)
            d = lower(c) - exp
            eq = 1 - Min(1, Max(d, 0 - d))
            add(m.rng(base + 1, base + n - 1),
                If([(COL, AC), (PR, ERR + (eq * (m + 1) + (1 - eq) * ND - ERR) * TB)], D()))
            add(m.eq(base + n), If([(COL, AC + kind), (PR, ERR + (ND - ERR) * TB)], D()))
        # ---- content-length: AC1, CLP, CLD, CLT, LCR1
        add(m.eq(AC + 1), If([(OWS, AC + 1), (c.eq(43), CLP), (DIG, CLD + c - 48)], D()))
        add(m.eq(CLP), If([(DIG, CLD + c - 48)], D()))
        acc = m - CLD
        add(m.rng(CLD, CLD + BIG),
            If([(DIG, CLD + Min(BIG, acc * 10 + c - 48)), (OWS, m + (CLT - CLD)),
                (CR, If([(m.gt(CLD + MAX_CL), ERR)], m + (LCR1 - CLD))),
                (LF, If([(m.gt(CLD + MAX_CL), ERR)], m + (K1 - CLD)))], D()))
        add(m.rng(CLT, CLT + BIG),
            If([(OWS, m), (CR, If([(m.gt(CLT + MAX_CL), ERR)], m + (LCR1 - CLT))),
                (LF, If([(m.gt(CLT + MAX_CL), ERR)], m + (K1 - CLT)))], D()))
        add(m.rng(LCR1, LCR1 + BIG), If([(LF, m + (K1 - LCR1))], D()))
        # ---- authorization: AC3, VA (and K4), VAW, LCRA, LCRE
        add(m.eq(AC + 3), If([(OWS, AC + 3), (NOWS, K4 + P), (CR, LCRE + P), (LF, K6 + P)], D()))
        add(m.eq(VA) | m.rng(K4, K4 + 9999),
            If([(NOWS, VA), (OWS, VAW + 1), (CR, LCRA + P), (LF, K5 + P)], D()))
        add(m.rng(VAW + 1, VAW + 255),
            If([(OWS, If([(m.eq(VAW + 255), ERR)], m + 1)), (NOWS, VA), (CR, LCRA + P - (m - VAW)), (LF, K5 + P - (m - VAW))],
               D()))
        add(m.rng(LCRA, LCRA + 9999), If([(LF, K5 + (m - LCRA))], D()))
        add(m.rng(LCRE, LCRE + 9999), If([(LF, K6 + (m - LCRE))], D()))
    if rl:
        # ---- request line
        add(m.eq(L0), If([(c.eq(71), G1), (c.eq(80), P1), (CR, L0CR), (LF, L0), (PR, MO)], D()))
        add(m.eq(L0CR), If([(LF, L0)], D()))
        for st, ch, nx in ((G1, 'E', G2), (G2, 'T', G3), (P1, 'O', P2), (P2, 'S', P3), (P3, 'T', P4)):
            add(m.eq(st), If([(c.eq(ord(ch)), nx), (SP, U0 + 2), (PR, MO)], D()))
        add(m.eq(G3), If([(SP, U0), (PR, MO)], D()))
        add(m.eq(P4), If([(SP, U0 + 1), (PR, MO)], D()))
        add(m.eq(MO), If([(SP, U0 + 2), (PR, MO)], D()))
        add(m.rng(U0, U0 + 2), If([(URIB, UR + 10 * (m - U0))], D()))
        for i, ch in enumerate('HTTP/1.'):
            add(m.eq(VH + i), If([(c.eq(ord(ch)), VH + i + 1)], D()))
        add(m.eq(VH + 7), If([(c.eq(48), VE0), (c.eq(49), VE1)], D()))
        add(m.eq(VE0) | m.eq(VE1), If([(CR, m + 2), (LF, HS)], D()))
        add(m.eq(VC0) | m.eq(VC1), If([(LF, HS)], D()))
    return If(cases, D())


# ------------------------------------------------------------------------------------------------ layer 2 helpers
def first_where(pred, leaf, lo=1, hi=S):
    """leaf(k) for the smallest k in lo..hi with pred(k) true; pred monotone, true at hi."""
    if lo == hi:
        return leaf(lo)
    mid = (lo + hi) // 2
    return If([(pred(mid), first_where(pred, leaf, lo, mid))], first_where(pred, leaf, mid + 1, hi))


def inband(q, lo, hi):
    return Min(1, Max(0, q - lo + 1)) * Min(1, Max(0, hi - q + 1))


def decode_last(fam, lo, hi):
    """max over the chunk of (q - lo + 1) for q in [lo, hi]; 0 = no marker."""
    return Max(*[inband(Q(fam, k), lo, hi) * (Q(fam, k) - lo + 1) for k in range(1, S + 1)])


def decode_first(fam, bands):
    """BIG - (smallest payload over the bands (lo, hi)); 0 = no marker."""
    return Max(*[inband(Q(fam, k), lo, hi) * (BIG + lo - Q(fam, k)) for k in range(1, S + 1) for lo, hi in bands])


def hs_term(q):
    return (1 - Min(1, Max(q - HS, HS - q))) + inband(q, K1, HSMAX)


ACTIVE = Not(Pr('--hmf').rng(DONE, ERR))
INACTIVE = Pr('--hmf').rng(DONE, ERR)


def other(par):
    return 'b' if par == 'a' else 'a'


def chain_decls(par, rl, sp, last=False):
    """q chain + the flags the child needs (ordered)."""
    ms = '--h%s%d' % (other(par), S)
    ds = []
    prev = ms
    for j in range(S):
        name = '--h%s%d' % (par, j + 1)
        ds.append((name, trans(Pr(prev), Pr('--h%d' % j), j, rl, sp, last)))
        prev = name
    q16 = Pr(prev)
    F = lambda n: '--%s%s' % (n, par)
    ds.append(('--hmf', q16 + 0))
    ds.append((F('hact'), K(1)))
    ds.append((F('hed'), If([(q16.eq(DONE), 1)], 0)))
    if rl:
        e = Pr(ms)
        ds.append((F('hea'), If([(e.lt(30) & q16.ge(30), 1)], 0)))
        ds.append((F('heb'), If([(e.lt(70) & q16.ge(70), 1)], 0)))
        ds.append((F('hec'), If([(e.lt(80) & q16.ge(80), 1)], 0)))
        ds.append((F('hev'), Pr(F('hea')) + Pr(F('heb')) + Pr(F('hec')) + Pr(F('hed'))))
    else:
        ds.append((F('hea'), K(0)))
        ds.append((F('heb'), K(0)))
        ds.append((F('hec'), K(0)))
        ds.append((F('hev'), Pr(F('hed')) + 0))
    if sp:
        ds.append((F('hmk'), Max(*[Pr('--h%s%d' % (par, k)) for k in range(1, S + 1)])))
    return ds


def pure_decls(par):
    ds = [('--h%s%d' % (par, j), K(VA)) for j in range(1, S + 1)]
    F = lambda n: '--%s%s' % (n, par)
    return ds + [('--hmf', K(VA)), (F('hact'), K(1)), (F('hed'), K(0)), (F('hea'), K(0)), (F('heb'), K(0)), (F('hec'), K(0)),
                 (F('hev'), K(0)), (F('hmk'), K(0))]


def chunk_class_string(ch):
    return class_string(ch)[1:-1]


def event_decls(fam):
    """Request-line / head-end registers, decoded by the child from its parent's q family `fam` (gated by --hev)."""
    q = lambda k: Q(fam, k)
    pk = lambda k: off - S + k
    hea, heb, hec, hed = (Pr('--%s%s' % (n, fam)) for n in ('hea', 'heb', 'hec', 'hed'))
    return [
        ('--hps', If([(hea.eq(1), first_where(lambda k: q(k).ge(30), lambda k: pk(k)))], Inh('--hps'))),
        ('--hmt', If([(hea.eq(1), first_where(lambda k: q(k).ge(30), lambda k: q(k) - 30))], Inh('--hmt'))),
        ('--hpe', If([(heb.eq(1), first_where(lambda k: q(k).ge(70), lambda k: pk(k) - 1))], Inh('--hpe'))),
        ('--hv0', If([(hec.eq(1), first_where(lambda k: q(k).ge(80), lambda k: If([(q(k).eq(80), 1)], 0)))], Inh('--hv0'))),
        ('--hvd', If([(hec.eq(1), 1)], Inh('--hvd'))),
        ('--hhl', If([(hed.eq(1), first_where(lambda k: q(k).eq(DONE), lambda k: pk(k)))], Inh('--hhl'))),
    ]


def cl_decls(fam):
    e1 = Pr('--he1')
    return [('--he1', decode_last(fam, K1, K1 + 49999)), ('--hcl', If([(e1.ge(1), e1 - 1)], Inh('--hcl')))]


def auth_decls(fam):
    """First Authorization header: `as` in the --has gate, `ae` in the --hae gate (each closes once set).
    he4 = K4 (value start), he5 / he6 = K5 (value end) / K6 (empty value) as BIG - payload (0 = none); he56 = the earlier of the two."""
    e4, e5, e6, e56 = (Pr(n) for n in ('--he4', '--he5', '--he6', '--he56'))
    pre = [('--he4', decode_first(fam, [(K4, K4 + 9999)])), ('--he5', decode_first(fam, [(K5, K5 + 9999)])),
           ('--he6', decode_first(fam, [(K6, K6 + 9999)])), ('--he56', Max(e5, e6)),
           ('--he4v', e4 * Min(1, Max(0, e4 - e56 + 1))),                      # the value start only counts before any line end
           ('--he6w', e6 * Min(1, Max(0, e6 - e5 + 1)))]                       # an empty value that ends the first line
    as_ = ('--has', If([(Pr('--he4v').ge(1), BIG - Pr('--he4v')), (Pr('--he6w').ge(1), BIG - Pr('--he6w'))], Inh('--has')))
    ae_ = ('--hae', If([(e56.ge(1), BIG - e56)], Inh('--hae')))
    return pre, as_, ae_


def hc_decls(par):
    """Header-line counter (heads of >= 208 bytes only): number of line starts entered, decoded from the parent's q family."""
    fam = other(par)
    t = Pr('--hc' + fam)
    for k in range(1, S + 1):
        t = t + hs_term(Q(fam, k))
    return [('--hc' + par, t), ('--hhcf', Pr('--hc' + par) + 0)]


# ------------------------------------------------------------------------------------------------ rules
class _Id(dict):
    def __missing__(self, k):
        return k


B = _Id()


def css_decls(ds):
    return ''.join('%s:%s;' % (n, render(e, B)) for n, e in ds)


def hs_decls():
    return [('--hbig', If([(Pr('--len').ge(HEAD_BIG), 1)], 0)), ('--hlen', Pr('--len') + 0)]


def rules():
    out = [selector_rules(), tier_s_rules(), '.Hs{%s}\n' % css_decls(hs_decls())]
    hint1, hint0 = Pr('--shint').eq(1), Pr('--shint').eq(0)
    vd0, vd1 = Pr('--hvd').eq(0), Pr('--hvd').eq(1)
    pure = ''.join(':not([dc*="%s"])' % x for x in 'srn!')       # a chunk of token characters only
    # NOTE: a declaration that loses the cascade is still processed, so the alternatives below must be mutually exclusive
    for rl, vdc in ((True, vd0), (False, vd1)):
        for sp, hc in ((False, hint0), (True, hint1)):
            variants = [(vdc & hc & ACTIVE, '')]
            if sp and not rl:       # in the middle of an authorization value the pure chunks skip the automaton
                variants = [(vdc & hc & Not(Pr('--hmf').rng(VA, VA)) & ACTIVE, ''), (vdc & hc & Pr('--hmf').rng(VA, VA), ':not([dc]%s)' % pure)]
                out.append('@container %s{\n' % cond(vdc & hc & Pr('--hmf').rng(VA, VA), B))
                for par, cls in (('a', 'H0'), ('b', 'H1')):
                    out.append('.%s[dc]%s{%s}\n' % (cls, pure, css_decls(pure_decls(par))))
                out.append('}\n')
            for gate, extra in variants:
                out.append('@container %s{\n' % cond(gate, B))
                for par, cls in (('a', 'H0'), ('b', 'H1')):
                    out.append('.%s:not(.Hl)%s{%s}\n' % (cls, extra, css_decls(chain_decls(par, rl, sp))))
                    out.append('.%s.Hl%s{%s}\n' % (cls, extra, css_decls(chain_decls(par, rl, sp, True))))   # the last chunk may be padded
                out.append('}\n')
    out.append('@container %s{.H0{--heva:0;--hmka:0;--hacta:0}.H1{--hevb:0;--hmkb:0;--hactb:0}}\n' % cond(INACTIVE, B))
    # events of the parent chunk, decoded by the child (H or Z of parity par reads the family of the other parity)
    for par in 'ab':
        fam, sel = other(par), '.H%d,.Z%d' % ((0, 0) if par == 'a' else (1, 1))
        out.append('@container style(--hev%s >= 1){%s{%s}}\n' % (fam, sel, css_decls(event_decls(fam))))
        out.append('@container style(--hmk%s >= %d){\n' % (fam, K4))
        out.append('%s{%s}\n' % (sel, css_decls(cl_decls(fam))))
        pre, as_, ae_ = auth_decls(fam)
        out.append('%s{%s}\n' % (sel, css_decls(pre)))
        out.append('@container style(--has:0){%s{%s}}\n' % (sel, css_decls([as_])))
        out.append('@container style(--hae:0){%s{%s}}\n' % (sel, css_decls([ae_])))
        out.append('}\n')
        out.append('@container style(--hbig:1) and style(--hact%s:1){%s{%s}}\n' % (fam, sel, css_decls(hc_decls(par))))
    out.append('.Z{%scounter-reset:%s}\n' % (css_decls([sok_decl()]), ' '.join('%s %s' % (n, render(e, B)) for n, e in z_outputs())))
    return ''.join(out)


def sok_decl():
    c = Pr('--scert').eq(1) & Pr('--sroute').eq(1) & Pr('--shint').eq(0) & Pr('--ste').eq(0) & Pr('--hbig').eq(0)
    return ('--sok', If([(c, 1)], 0))


def z_outputs():
    mf, hcf = Pr('--hmf'), Pr('--hhcf')
    as_, ae = Pr('--has'), Pr('--hae')
    sok = Pr('--sok')
    bad = If([(mf.eq(ERR), 1), (hcf.ge(66), 1), (Pr('--ste').eq(1), 1)], 0)
    ok = If([(Pr('--ste').eq(1), 0), (mf.eq(DONE) & hcf.lt(66), 1)], 0)
    v0 = If([(sok.eq(1), Pr('--sv0'))], Pr('--hv0'))
    S_ = lambda x, e: If([(sok.eq(1), x)], e)
    return [
        ('ok', S_(1, ok)), ('bad', S_(0, bad)), ('hl', S_(Pr('--hlen'), Pr('--hhl') + 0)), ('cl', S_(0, Pr('--hcl') + 0)),
        ('used', S_(Pr('--hlen'), Pr('--hhl') + Pr('--hcl'))),
        ('close', If([(Pr('--scc').eq(1), 1), (Pr('--sck').eq(1), 0)], v0)), ('cont', Pr('--sex') + 0),
        ('mt', S_(0, Pr('--hmt') + 0)), ('ps', S_(Pr('--sps'), Pr('--hps') + 0)), ('pe', S_(Pr('--spe'), Pr('--hpe') + 0)),
        ('as', S_(0, If([(as_.eq(0), ae)], as_))), ('ae', S_(0, ae + 0)), ('slow', 1 - sok),
    ]


def properties():
    return REG.listing()


# ------------------------------------------------------------------------------------------------ python simulation
INITIAL = {n: v for n, s, i, v in REG.listing()}
INHERITED = [n for n, s, i, v in REG.listing() if i]


def _run(ds, env):
    for n, e in ds:
        env[n] = evaluate(e, env)


def simulate(window):
    """Z outputs for a head window (bytes), evaluating the generated declarations like the chained elements would."""
    chunks = [window[i:i + S] for i in range(0, len(window), S)] or [b'']
    pen = {n: INITIAL[n] for n in INITIAL}                     # the parent's computed values (Hs)
    pen.update(sim_flags(window))
    pen.update(sim_tier_s(window))
    pen['--len'] = len(window)
    env = dict(pen)
    _run(hs_decls(), env)
    pen = env
    for k in range(len(chunks) + 1):
        isz = k == len(chunks)
        par = 'a' if k % 2 == 0 else 'b'
        fam = other(par)
        env = {n: INITIAL[n] for n in INITIAL}
        for n in INHERITED:
            env[n] = pen[n]
            env['inh:' + n] = pen[n]
        env['--off'] = k * S
        if not isz:
            ch = chunks[k]
            for i in range(S):
                env['--h%d' % i] = ch[i] if i < len(ch) else 256
            if test(INACTIVE, pen):
                env['--hact' + par] = env['--hev' + par] = env['--hmk' + par] = 0
            else:
                rl = test(Pr('--hvd').eq(0), pen)
                sp = test(Pr('--shint').eq(1), pen)
                dc = chunk_class_string(ch)
                if not rl and sp and pen['--hmf'] == VA and not any(x in dc for x in 'srn!'):
                    _run(pure_decls(par), env)
                else:
                    _run(chain_decls(par, rl, sp, k == len(chunks) - 1), env)
        if test(Pr('--hev' + fam).ge(1), pen):
            _run(event_decls(fam), env)
        if test(Pr('--hmk' + fam).ge(K4), pen):
            _run(cl_decls(fam), env)
            pre, as_, ae_ = auth_decls(fam)
            _run(pre, env)
            if test(Pr('--has').eq(0), pen):
                _run([as_], env)
            if test(Pr('--hae').eq(0), pen):
                _run([ae_], env)
        if test(Pr('--hbig').eq(1) & Pr('--hact' + fam).eq(1), pen):
            _run(hc_decls(par), env)
        pen = env
    _run([sok_decl()], pen)
    return {n: evaluate(e, pen) for n, e in z_outputs()}


if __name__ == '__main__':
    css = rules()
    print(len(css), 'bytes of rules')
