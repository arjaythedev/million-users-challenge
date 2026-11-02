#!/usr/bin/env python3
"""SHA-256 compression function as CSS (Python 3 stdlib only).

`compress_rule()` emits one CSS rule that, on every element matching `selector`, computes one SHA-256 compression:
it reads the chaining state from `--<in_prefix><k>l/h` (inherited, registered `<integer>` props set by an ancestor)
or the standard IV, reads the 16 message words, and writes the new chaining state to `--<out_prefix><k>l/h`.

Number format (measured on Chrome 155; do not change lightly)
  * Chrome serializes every computed custom-property value to text when another declaration reads it through
    var(), and serializes calc() results with 6 significant digits. So every value that crosses a custom-property
    boundary must have at most 6 significant digits (|v| <= 999999 is always exact). Inside ONE calc() everything
    is a double (exact below 2^53) and can be as large as needed.
  * Hence a 32-bit word is a pair of 16-bit halves `<name>l` (bits 0..15) and `<name>h` (bits 16..31), both
    registered `<integer>`, in 0..65535. State word k (k = 0..7 = a..h) of prefix P is `--P<k>l` / `--P<k>h`.
  * Bit i of a word is `mod(round(down, half / 2^(i mod 16)), 2)`, extracted once into its own non-inherited
    registered prop and then referenced (var() references dominate the cost, ~0.3 us each).

API
  compress_rule(selector, in_prefix, out_prefix, msg=..., tag=..., wrap=...) -> str    one rule
  property_rules() -> str          every @property registration needed so far (call after the last compress_rule)
  reset()                          forget registrations (tests)
  state_words(prefix) -> list      8 (lo_css, hi_css) tuples for state words, usable as `msg` items
  to_halves(u32) / from_halves(l, h) / bytes_to_words(b) / words_to_bytes(ws) / reference_compress(state, block)

`msg` selects the 16 message words W0..W15:
  * ('bytes', '--c')  (default) 64 byte props `--c0..--c63` on the element (integers 0..255), big endian.
  * a list of 16 items, each an int (constant 32-bit word) or a (lo_css, hi_css) tuple of CSS expressions for the
    16-bit halves (e.g. state_words('ia')[3]).
Options: `bits` 'prop' (default; best, measured) | 'inline' | 'macro' (benchmarks only); `short` short intermediate
prop names (smaller CSS); `bare` omit the calc() wrapper when a declaration is a single function call.
Two rules that match the SAME element must use different `tag`s (their intermediates would collide).
`in_prefix=None` starts from the IV (or `iv=[8 ints]`, e.g. a midstate known at build time).
"""
import sys

K = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]
IV = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]


def to_halves(u):
    return u & 0xFFFF, (u >> 16) & 0xFFFF


def from_halves(l, h):
    return ((h & 0xFFFF) << 16) | (l & 0xFFFF)


def bytes_to_words(b):
    return [int.from_bytes(b[i:i + 4], 'big') for i in range(0, len(b), 4)]


def words_to_bytes(ws):
    return b''.join((w & 0xFFFFFFFF).to_bytes(4, 'big') for w in ws)


def reference_compress(state, block):
    rotr = lambda x, n: ((x >> n) | (x << (32 - n))) & 0xFFFFFFFF
    w = bytes_to_words(block)
    for i in range(16, 64):
        s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3)
        s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10)
        w.append((w[i - 16] + s0 + w[i - 7] + s1) & 0xFFFFFFFF)
    a, b, c, d, e, f, g, h = state
    for i in range(64):
        t1 = (h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i]) & 0xFFFFFFFF
        t2 = ((rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c))) & 0xFFFFFFFF
        h, g, f, e, d, c, b, a = g, f, e, (d + t1) & 0xFFFFFFFF, c, b, a, (t1 + t2) & 0xFFFFFFFF
    return [(x + y) & 0xFFFFFFFF for x, y in zip((a, b, c, d, e, f, g, h), state)]


# ---------------------------------------------------------------------------------------------------------------
_registered = {}   # prop name -> inherits(bool)


def reset():
    _registered.clear()


def _reg(name, inherits=False):
    _registered[name] = _registered.get(name, False) or inherits


def property_rules():
    return ''.join('@property %s{syntax:"<integer>";inherits:%s;initial-value:0}\n'
                   % (n, 'true' if inh else 'false') for n, inh in sorted(_registered.items()))


def property_list():
    """The same registrations as property_rules(), as (name, syntax, inherits, initial) tuples."""
    return [(n, '<integer>', inh, 0) for n, inh in sorted(_registered.items())]


def byte_xor_const(expr, m):
    """CSS expression for (byte XOR m); `expr` is CSS text for a byte (0..255), m a python int constant."""
    if m == 0:
        return expr
    flips = [k for k in range(8) if (m >> k) & 1]
    terms = ' + '.join('%d*mod(round(down,%s/%d),2)' % (1 << k, expr, 1 << k) if k else 'mod(%s,2)' % expr for k in flips)
    return '(%d + %s - 2*(%s))' % (m, expr, terms)


def words_from_byte_exprs(bytes_css):
    """64 CSS byte expressions (big endian) -> 16 (lo_css, hi_css) message-word tuples for `msg=`."""
    assert len(bytes_css) == 64
    return [('%s*256 + %s' % (bytes_css[4 * t + 2], bytes_css[4 * t + 3]),
             '%s*256 + %s' % (bytes_css[4 * t], bytes_css[4 * t + 1])) for t in range(16)]


def digest_block_msg(prefix, total_len_bytes):
    """Message words for a single padded block whose payload is a 32-byte digest held in state `prefix`
    (e.g. HMAC outer block: total_len_bytes = 64 + 32)."""
    return state_words(prefix) + [0x80000000, 0, 0, 0, 0, 0, 0, total_len_bytes * 8]


def state_words(prefix):
    return [('var(--%s%dl)' % (prefix, k), 'var(--%s%dh)' % (prefix, k)) for k in range(8)]


class _Word:
    """A 32-bit word: lo/hi are CSS text for the two 16-bit halves, or ints when constant."""
    __slots__ = ('lo', 'hi', 'name')

    def __init__(self, lo, hi, name):
        self.lo, self.hi, self.name = lo, hi, name

    def half(self, h):
        return self.hi if h else self.lo


class _Gen:
    def __init__(self, tag, bits, short=False, bare=False):
        self.tag, self.bits, self.short, self.bare = tag, bits, short, bare
        self.names = {}
        self.decls = []
        self.bitcache = {}

    def n(self, s):
        if self.short:
            v = self.names.get(s)
            if v is None:
                v = self.names[s] = '--%s_%s' % (self.tag, _b36(len(self.names)))
            return v
        return '--%s%s' % (self.tag, s)

    def prop(self, name, expr):
        if self.bare and expr.startswith(('mod(', 'round(')) and expr.count('(') == expr.count(')') and _single(expr):
            self.decls.append('%s:%s;' % (name, expr))
        else:
            self.decls.append('%s:calc(%s);' % (name, expr))
        _reg(name)
        return 'var(%s)' % name

    def bit(self, w, i):
        """0/1 expression for bit i of word w (python int if w is constant)."""
        hv = w.half(i >> 4)
        k = i & 15
        if isinstance(hv, int):
            return (hv >> k) & 1
        key = (w.name, i)
        if key in self.bitcache:
            return self.bitcache[key]
        if k == 15:
            e = 'round(down,%s/32768)' % hv
        elif k:
            e = 'mod(round(down,%s/%d),2)' % (hv, 1 << k)
        else:
            e = 'mod(%s,2)' % hv
        if self.bits == 'prop':
            r = self.prop(self.n('%s_%d' % (w.name, i)), e)
        elif self.bits == 'macro':      # unregistered text macro: no evaluation, expanded at each use
            nm = self.n('%s_%d' % (w.name, i))
            self.decls.append('%s:%s;' % (nm, e))
            r = 'var(%s)' % nm
        else:
            r = e
        self.bitcache[key] = r
        return r

    def new_word(self, name, lo_expr, hi_expr):
        """Create word props: lo = lo_expr, hi = hi_expr (expressions already reduced to 0..65535)."""
        lo = self.prop(self.n(name + 'l'), lo_expr)
        hi = self.prop(self.n(name + 'h'), hi_expr)
        return _Word(lo, hi, name)


def _par(e):
    """Parenthesize a CSS expression unless it is a single var() / number."""
    e = e.strip()
    if e.startswith('var(') and e.endswith(')') and e.count('(') == 1:
        return e
    if e.lstrip('-').isdigit():
        return e
    return '(%s)' % e


def _b36(n):
    d = '0123456789abcdefghijklmnopqrstuvwxyz'
    r = ''
    while True:
        r = d[n % 36] + r
        n //= 36
        if not n:
            return r


def _single(expr):
    """True if expr is one function call spanning the whole text (so calc() wrapper is unnecessary)."""
    depth = 0
    for i, ch in enumerate(expr):
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
            if depth == 0 and i != len(expr) - 1:
                return False
    return True


def _s(*xs):
    """Join terms with ' + ' dropping python-int zeros; fold int constants."""
    const = sum(x for x in xs if isinstance(x, int))
    parts = [x for x in xs if not isinstance(x, int)]
    if const or not parts:
        parts.append(str(const))
    return ' + '.join(parts)


def _xor3(x, y, z):
    ints = [v for v in (x, y, z) if isinstance(v, int)]
    if len(ints) == 3:
        return x ^ y ^ z
    return 'mod(%s,2)' % _s(x, y, z)


def _xor2(x, y):
    return 'mod(%s,2)' % _s(x, y)


def compress_rule(selector, in_prefix, out_prefix, msg=('bytes', '--c'), tag='h', bits='prop', wrap=None,
                  iv=None, short=True, bare=True):
    """Return the CSS rule text (see module docstring). `tag` namespaces the non-inherited intermediates:
    rules with the same tag share registrations (fine, they are non-inherited and recomputed per element)."""
    g = _Gen(tag, bits, short, bare)
    M = 65536

    # ---- chaining input words (a..h) ----
    if in_prefix is None:
        iv = iv or IV
        inw = [_Word(*to_halves(x), 'iv%d' % k) for k, x in enumerate(iv)]
    else:
        inw = [_Word('var(--%s%dl)' % (in_prefix, k), 'var(--%s%dh)' % (in_prefix, k), 'in%d' % k) for k in range(8)]

    # ---- message words ----
    W = {}
    if isinstance(msg, tuple):
        assert msg[0] == 'bytes'
        pre = msg[1]
        for t in range(16):
            c = ['var(%s%d)' % (pre, 4 * t + j) for j in range(4)]
            W[t] = g.new_word('W%d' % t, '%s*256 + %s' % (c[2], c[3]), '%s*256 + %s' % (c[0], c[1]))
    else:
        assert len(msg) == 16
        for t, m in enumerate(msg):
            if isinstance(m, int):
                W[t] = _Word(m & 0xFFFF, (m >> 16) & 0xFFFF, 'Wc%d' % t)
            else:
                W[t] = _Word(_par(m[0]), _par(m[1]), 'Wx%d' % t)

    # ---- schedule: W[t] = s1(W[t-2]) + W[t-7] + s0(W[t-15]) + W[t-16] ----
    for t in range(16, 64):
        x, y = W[t - 15], W[t - 2]
        sums = []
        for h in (0, 1):
            terms = []
            for k in range(16):
                i = h * 16 + k
                s0 = [g.bit(x, (i + 7) % 32), g.bit(x, (i + 18) % 32)]
                if i + 3 < 32:
                    s0.append(g.bit(x, i + 3))
                s1 = [g.bit(y, (i + 17) % 32), g.bit(y, (i + 19) % 32)]
                if i + 10 < 32:
                    s1.append(g.bit(y, i + 10))
                e0 = _xor3(*s0) if len(s0) == 3 else _xor2(*s0)
                e1 = _xor3(*s1) if len(s1) == 3 else _xor2(*s1)
                terms.append('%d*(%s + %s)' % (1 << k, e0, e1) if k else '%s + %s' % (e0, e1))
            sums.append(_s(W[t - 7].half(h), W[t - 16].half(h)) + ' + ' + ' + '.join(terms))
        rl = g.prop(g.n('rW%dl' % t), sums[0])
        rh = g.prop(g.n('rW%dh' % t), sums[1])
        W[t] = g.new_word('W%d' % t, 'mod(%s,%d)' % (rl, M), 'mod(%s + round(down,%s/%d),%d)' % (rh, rl, M, M))

    # ---- rounds ----
    A, E = {}, {}
    for j, t in enumerate((0, -1, -2, -3)):
        A[t], E[t] = inw[j], inw[4 + j]
    for t in range(64):
        e, f, gg = E[t], E[t - 1], E[t - 2]
        a, b, c = A[t], A[t - 1], A[t - 2]
        kk = to_halves(K[t])
        # T1 raw halves: h + K + W + G + sum 2^k (S1_i + e_i (f_i - g_i))
        t1 = []
        for hf in (0, 1):
            terms = []
            for k in range(16):
                i = hf * 16 + k
                s1 = _xor3(g.bit(e, (i + 6) % 32), g.bit(e, (i + 11) % 32), g.bit(e, (i + 25) % 32))
                ch = '%s*(%s - %s)' % (g.bit(e, i), g.bit(f, i), g.bit(gg, i))
                terms.append('%d*(%s + %s)' % (1 << k, s1, ch) if k else '%s + %s' % (s1, ch))
            base = _s(E[t - 3].half(hf), kk[hf], W[t].half(hf), gg.half(hf))
            t1.append(g.prop(g.n('t%d%s' % (t, 'lh'[hf])), base + ' + ' + ' + '.join(terms)))
        # S0 + Maj raw sums combined with T1 raw halves
        s2 = []
        for hf in (0, 1):
            terms = []
            for k in range(16):
                i = hf * 16 + k
                s0 = _xor3(g.bit(a, (i + 2) % 32), g.bit(a, (i + 13) % 32), g.bit(a, (i + 22) % 32))
                mj = 'round(down,(%s + %s + %s)/2)' % (g.bit(a, i), g.bit(b, i), g.bit(c, i))
                terms.append('%d*(%s + %s)' % (1 << k, s0, mj) if k else '%s + %s' % (s0, mj))
            s2.append(g.prop(g.n('s%d%s' % (t, 'lh'[hf])), '%s + ' % t1[hf] + ' + '.join(terms)))
        d, hh = A[t - 3], E[t - 3]
        # E[t+1] = d + T1 ; A[t+1] = T1 + T2
        el = _s(d.lo, t1[0])
        eh = _s(d.hi, t1[1])
        E[t + 1] = g.new_word('E%d' % (t + 1), 'mod(%s,%d)' % (el, M),
                              'mod(%s + round(down,(%s)/%d),%d)' % (eh, el, M, M))
        A[t + 1] = g.new_word('A%d' % (t + 1), 'mod(%s,%d)' % (s2[0], M),
                              'mod(%s + round(down,%s/%d),%d)' % (s2[1], s2[0], M, M))
        if t == 63:
            break

    # ---- output ----
    final = [A[64], A[63], A[62], A[61], E[64], E[63], E[62], E[61]]
    outs = []
    for k in range(8):
        sl = _s(final[k].lo, inw[k].lo)
        sh = _s(final[k].hi, inw[k].hi)
        ol = '--%s%dl' % (out_prefix, k)
        oh = '--%s%dh' % (out_prefix, k)
        outs.append('%s:calc(mod(%s,%d));' % (ol, sl, M))
        outs.append('%s:calc(mod(%s + round(down,(%s)/%d),%d));' % (oh, sh, sl, M, M))
        _reg(ol, True)
        _reg(oh, True)
    rule = '%s{%s}' % (selector, ''.join(g.decls) + ''.join(outs))
    if wrap:
        rule = '%s{%s}' % (wrap, rule)
    return rule + '\n'


def pad_blocks(msg, prefix_len=0):
    """SHA-256 padding of `msg` (bytes) as a list of 64-byte blocks; `prefix_len` bytes were already compressed
    (64 for the HMAC inner hash after the ipad block)."""
    p = msg + b'\x80' + b'\x00' * ((55 - len(msg)) % 64) + ((prefix_len + len(msg)) * 8).to_bytes(8, 'big')
    return [p[i:i + 64] for i in range(0, len(p), 64)]


def _selftest():
    import hashlib
    import hmac
    import os
    for n in (0, 1, 55, 56, 63, 64, 119, 120, 200):
        m = os.urandom(n)
        st = list(IV)
        for blk in pad_blocks(m):
            st = reference_compress(st, blk)
        assert words_to_bytes(st) == hashlib.sha256(m).digest(), n
    key = b'k' * 23
    kb = key.ljust(64, b'\0')
    m = os.urandom(135)
    ist = reference_compress(list(IV), bytes(x ^ 0x36 for x in kb))
    for blk in pad_blocks(m, 64):
        ist = reference_compress(ist, blk)
    ost = reference_compress(list(IV), bytes(x ^ 0x5c for x in kb))
    for blk in pad_blocks(words_to_bytes(ist), 64):
        ost = reference_compress(ost, blk)
    assert words_to_bytes(ost) == hmac.new(key, m, hashlib.sha256).digest()
    r = compress_rule('.S', 'a', 'b')
    assert r.count('{') == r.count('}') and 'calc(' in r
    print('selftest ok: reference model matches hashlib/hmac; rule is %d KB, %d props' % (len(r) // 1024, len(_registered)))


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser(description='emit one demo compression rule (.S, in prefix "in", out prefix "out") or self-test')
    ap.add_argument('--bits', default='prop')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        _selftest()
    else:
        sys.stdout.write(compress_rule('.S', 'in', 'out', bits=a.bits))
        sys.stdout.write(property_rules())
