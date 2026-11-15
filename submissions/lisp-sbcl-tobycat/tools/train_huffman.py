# Trains a DEFLATE dynamic-Huffman code on feed bodies, using the same greedy LZ77 as src/gzip.lisp.
# usage: train_huffman.py BODY_DIR OUT_LISP [MORE_BODY_DIRS]   (each dir holds *.json feed bodies, equal weight each)
# Writes src/gzip-tables.lisp and prints size comparisons.
import sys, os, zlib, struct, glob

LEN_BASE = [3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258]
LEN_EXTRA = [0]*8 + [1]*4 + [2]*4 + [3]*4 + [4]*4 + [5]*4 + [0]
DIST_BASE = [1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577]
DIST_EXTRA = [0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13]

def len_sym(l):
    for i in range(28, -1, -1):
        if l >= LEN_BASE[i]: return 257 + i, LEN_EXTRA[i], l - LEN_BASE[i]
def dist_sym(d):
    for i in range(29, -1, -1):
        if d >= DIST_BASE[i]: return i, DIST_EXTRA[i], d - DIST_BASE[i]

def lz77(b):
    """Tokens exactly as gzip-into in src/gzip.lisp makes them: ('L', byte) or ('M', length, distance)."""
    n = len(b); head = {}; out = []; i = 0; hend = n - 4
    def h(j): return (((struct.unpack_from('<I', b, j)[0] & 0xffffff) * 2654435761) & 0xffffffff) >> 19
    while i < n:
        best = 0
        if i <= hend:
            k = h(i); c = head.get(k); head[k] = i
            if c is not None and i - c <= 32768 and b[c:c+3] == b[i:i+3]:
                lim = min(258, n - i); l = 3
                while l < lim and b[c+l] == b[i+l]: l += 1
                best = l; dist = i - c
        if best >= 3:
            out.append(('M', best, dist))
            if best <= 4:
                for j in range(i + 1, min(i + best, hend + 1)): head[h(j)] = j
            i += best
        else:
            out.append(('L', b[i])); i += 1
    return out

def package_merge(freqs, limit):
    """Length-limited Huffman code lengths (freqs > 0 for every symbol that needs a code)."""
    syms = [(f, [i]) for i, f in enumerate(freqs) if f > 0]
    if len(syms) == 1: return [1 if f > 0 else 0 for f in freqs]
    syms.sort()
    pkgs = list(syms)
    for _ in range(limit - 1):
        merged = [(pkgs[i][0] + pkgs[i+1][0], pkgs[i][1] + pkgs[i+1][1]) for i in range(0, len(pkgs) - 1, 2)]
        pkgs = sorted(syms + merged, key=lambda x: x[0])
    lens = [0] * len(freqs)
    for _, s in pkgs[:2 * len(syms) - 2]:
        for i in s: lens[i] += 1
    return lens

def canon(lens):
    """Canonical codes, bit-reversed for an LSB-first writer."""
    maxl = max(lens); bl = [0] * (maxl + 1)
    for l in lens:
        if l: bl[l] += 1
    code = 0; nxt = [0] * (maxl + 2)
    for b in range(1, maxl + 1):
        code = (code + bl[b-1]) << 1 if b > 1 else 0
        nxt[b] = code
    # recompute properly per RFC 1951
    code = 0; bl[0] = 0
    for b in range(1, maxl + 1):
        code = (code + bl[b-1]) << 1
        nxt[b] = code
    codes = [0] * len(lens)
    for i, l in enumerate(lens):
        if l:
            c = nxt[l]; nxt[l] += 1
            codes[i] = int(format(c, f'0{l}b')[::-1], 2)
    return codes

class Bits:
    def __init__(s): s.v = 0; s.n = 0
    def put(s, val, n): s.v |= val << s.n; s.n += n

def header_bits(ll_lens, d_lens):
    """BFINAL=1, BTYPE=2 and the code-length-coded tables."""
    allv = ll_lens + d_lens
    rle = []  # (sym, extra_bits, extra_val)
    i = 0
    while i < len(allv):
        v = allv[i]; r = 1
        while i + r < len(allv) and allv[i + r] == v: r += 1
        if v == 0:
            k = r
            while k >= 11: t = min(k, 138); rle.append((18, 7, t - 11)); k -= t
            while k >= 3: t = min(k, 10); rle.append((17, 3, t - 3)); k -= t
            rle += [(0, 0, 0)] * k
        else:
            rle.append((v, 0, 0)); k = r - 1
            while k >= 3: t = min(k, 6); rle.append((16, 2, t - 3)); k -= t
            rle += [(v, 0, 0)] * k
        i += r
    cf = [0] * 19
    for s, _, _ in rle: cf[s] += 1
    cl = package_merge(cf, 7); cc = canon(cl)
    order = [16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15]
    hclen = 19
    while hclen > 4 and cl[order[hclen-1]] == 0: hclen -= 1
    b = Bits()
    b.put(1, 1); b.put(2, 2)
    b.put(len(ll_lens) - 257, 5); b.put(len(d_lens) - 1, 5); b.put(hclen - 4, 4)
    for k in range(hclen): b.put(cl[order[k]], 3)
    for s, eb, ev in rle:
        b.put(cc[s], cl[s])
        if eb: b.put(ev, eb)
    return b

def encode(body, ll_codes, ll_lens, d_codes, d_lens, hdr):
    b = Bits(); b.v = hdr.v; b.n = hdr.n
    for t in lz77(body):
        if t[0] == 'L': b.put(ll_codes[t[1]], ll_lens[t[1]])
        else:
            s, eb, ev = len_sym(t[1]); b.put(ll_codes[s], ll_lens[s]); b.put(ev, eb)
            s, eb, ev = dist_sym(t[2]); b.put(d_codes[s], d_lens[s]); b.put(ev, eb)
    b.put(ll_codes[256], ll_lens[256])
    raw = b.v.to_bytes((b.n + 7) // 8, 'little')
    return (b'\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff' + raw +
            struct.pack('<II', zlib.crc32(body) & 0xffffffff, len(body) & 0xffffffff))

def main():
    bdir, out = sys.argv[1], sys.argv[2]
    dirs = [bdir] + sys.argv[3:]            # further corpora, equal weight each
    files = sorted(glob.glob(os.path.join(bdir, '*.json')))
    bodies = [open(f, 'rb').read() for f in files]
    corp = [[open(f, 'rb').read() for f in sorted(glob.glob(os.path.join(d, '*.json')))] for d in dirs]
    test = bodies[1::2]
    lf = [0.0] * 286; df = [0.0] * 30
    for c in corp:
        l1 = [0] * 286; d1 = [0] * 30
        for body in c[0::2]:
            for t in lz77(body):
                if t[0] == 'L': l1[t[1]] += 1
                else: l1[len_sym(t[1])[0]] += 1; d1[dist_sym(t[2])[0]] += 1
            l1[256] += 1
        tot = sum(l1) + sum(d1)
        for i in range(286): lf[i] += l1[i] / tot
        for i in range(30): df[i] += d1[i] / tot
    # every symbol stays encodable: any byte, any length, any distance
    lf = [int(f * 4000000) + 1 for f in lf]; df = [int(f * 4000000) + 1 for f in df]
    ll_lens = package_merge(lf, 15); d_lens = package_merge(df, 11)
    assert all(l + DIST_EXTRA[i] <= 24 for i, l in enumerate(d_lens))
    ll_codes = canon(ll_lens); d_codes = canon(d_lens)
    hdr = header_bits(ll_lens, d_lens)
    sz = {'dyn': 0, 'z6': 0}
    for body in test:
        g = encode(body, ll_codes, ll_lens, d_codes, d_lens, hdr)
        assert zlib.decompress(g, 31) == body
        sz['dyn'] += len(g); c = zlib.compressobj(6, zlib.DEFLATED, 31); sz['z6'] += len(c.compress(body) + c.flush())
    # robustness: arbitrary bytes round-trip too
    import random; random.seed(3)
    for _ in range(200):
        r = bytes(random.randrange(256) for _ in range(random.randrange(1, 9000)))
        assert zlib.decompress(encode(r, ll_codes, ll_lens, d_codes, d_lens, hdr), 31) == r
    fl=[8]*144+[9]*112+[7]*24+[8]*6; fd=[5]*30; fh=Bits(); fh.put(1,1); fh.put(1,2)
    for d, c in zip(dirs, corp):
        t = c[1::2]; a = sum(len(encode(b, ll_codes, ll_lens, d_codes, d_lens, hdr)) for b in t) / len(t)
        f = sum(len(encode(b, canon(fl), fl, canon(fd), fd, fh)) for b in t) / len(t)
        z = 0
        for b in t: co = zlib.compressobj(6, zlib.DEFLATED, 31); z += len(co.compress(b) + co.flush())
        print(f'{d}: raw {sum(map(len,t))/len(t):.0f}  fixed {f:.0f}  trained {a:.0f}  zlib-6 {z/len(t):.0f}')
    nt = len(test)
    print(f'held-out feeds: trained {sz["dyn"]/nt:.0f} B, zlib -6 {sz["z6"]/nt:.0f} B; header {hdr.n} bits')
    # tables, as Lisp
    def vec(name, vals, doc):
        rows = [' '.join(str(v) for v in vals[i:i+16]) for i in range(0, len(vals), 16)]
        return (';;; %s\n(sb-ext:define-load-time-global %s\n  (coerce #(%s)\n          (quote (simple-array (unsigned-byte 32) (*)))))\n'
                % (doc, name, '\n            '.join(rows)))
    ln = [0, 0, 0]; lnn = [0, 0, 0]
    for l in range(3, 259):
        sy, eb, ev = len_sym(l)
        ln.append(ll_codes[sy] | ev << ll_lens[sy]); lnn.append(ll_lens[sy] + eb)
    nb = hdr.n // 8
    hb = list((hdr.v & ((1 << (nb * 8)) - 1)).to_bytes(nb, 'little'))
    L = [';;;; Generated by tools/train_huffman.py: a DEFLATE dynamic-Huffman code trained on /feed bodies.',
         ';;;; Codes are bit-reversed for an LSB-first writer. Every byte, length and distance has a code.',
         '', '(in-package :mu)', '',
         vec('+gz-lit-code+', ll_codes[:257], 'literal bytes 0..255 and end-of-block (256)'),
         vec('+gz-lit-len+', ll_lens[:257], 'their code lengths'),
         vec('+gz-len-code+', ln, 'match length 3..258: its code with the extra bits behind it'),
         vec('+gz-len-len+', lnn, 'bits in that'),
         vec('+gz-dist-code+', d_codes, 'distance codes 0..29'),
         vec('+gz-dist-len+', d_lens, 'their code lengths'),
         ';;; the block header (BFINAL, BTYPE=10 and the code lengths): whole bytes, then the bits left over',
         "(sb-ext:define-load-time-global +gz-block-hdr+\n  (coerce #(%s)\n          (quote (simple-array (unsigned-byte 8) (*)))))" % ' '.join(map(str, hb)),
         '(defconstant +gz-hdr-tail+ %d)' % (hdr.v >> (nb * 8)),
         '(defconstant +gz-hdr-tail-bits+ %d)' % (hdr.n % 8)]
    open(out, 'w').write('\n'.join(L) + '\n')
    rd = os.environ.get('REF_DIR')
    if rd:
        os.makedirs(rd, exist_ok=True)
        for d, c in zip(dirs, corp):
            for k, body in enumerate(c[:40]):
                open(os.path.join(rd, '%s-%d.gz' % (os.path.basename(d.rstrip('/')), k)), 'wb').write(
                    encode(body, ll_codes, ll_lens, d_codes, d_lens, hdr))

main()
