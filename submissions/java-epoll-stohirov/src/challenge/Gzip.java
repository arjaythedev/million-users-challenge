package challenge;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.zip.CRC32;

final class Gzip {
    private static final byte[] HEADER = {0x1f, (byte) 0x8b, 8, 0, 0, 0, 0, 0, 0, (byte) 0xff};
    private static final VarHandle I32 = MethodHandles.byteArrayViewVarHandle(int[].class, ByteOrder.LITTLE_ENDIAN);
    private static final VarHandle I64 = MethodHandles.byteArrayViewVarHandle(long[].class, ByteOrder.LITTLE_ENDIAN);

    private static final int WINDOW = 32768, MIN_MATCH = 4, MAX_MATCH = 258, HASH_BITS = 14;
    private static final int LITLEN = 286, DIST = 30, CODELEN = 19, EOB = 256, MAX_BITS = 15, MAX_CL_BITS = 7;
    private static final int[] LEN_BASE = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67,
        83, 99, 115, 131, 163, 195, 227, 258};
    private static final int[] LEN_EXTRA = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5,
        5, 5, 0};
    private static final int[] DIST_BASE = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    private static final int[] DIST_EXTRA = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11,
        11, 12, 12, 13, 13};
    private static final int[] CODELEN_ORDER = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    private static final int[] CODELEN_EXTRA = {2, 3, 7};

    private static final byte[] LEN_CODE = new byte[MAX_MATCH + 1], DIST_CODE = new byte[512];
    private static final byte[] FIXED_LIT_LEN = new byte[288], FIXED_DIST_LEN = new byte[32];
    private static final int[] FIXED_LIT_CODE = new int[288], FIXED_DIST_CODE = new int[32];

    static {
        for (int c = 0; c < LEN_BASE.length; c++) {
            int to = c + 1 < LEN_BASE.length ? LEN_BASE[c + 1] : MAX_MATCH + 1;
            for (int l = LEN_BASE[c]; l < to; l++) LEN_CODE[l] = (byte) c;
        }
        for (int c = 0; c < DIST; c++) {
            for (int d = DIST_BASE[c] - 1, to = d + (1 << DIST_EXTRA[c]); d < to; d++) {
                DIST_CODE[d < 256 ? d : 256 + (d >>> 7)] = (byte) c;
            }
        }
        for (int s = 0; s < 288; s++) FIXED_LIT_LEN[s] = (byte) (s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8);
        Arrays.fill(FIXED_DIST_LEN, (byte) 5);
        codes(FIXED_LIT_LEN, 288, FIXED_LIT_CODE, new int[MAX_BITS + 2]);
        codes(FIXED_DIST_LEN, 32, FIXED_DIST_CODE, new int[MAX_BITS + 2]);
    }

    private final CRC32 crc = new CRC32();
    private final byte[] in;
    final byte[] buf;
    private int size;

    private final int[] head = new int[1 << HASH_BITS];
    private int base = 1;
    private final int[] tokens;

    private final int[] litFreq = new int[LITLEN], distFreq = new int[DIST], clFreq = new int[CODELEN];
    private final byte[] litLen = new byte[LITLEN], distLen = new byte[DIST], clLen = new byte[CODELEN];
    private final int[] litCode = new int[LITLEN], distCode = new int[DIST], clCode = new int[CODELEN];
    private final byte[] lens = new byte[LITLEN + DIST];
    private final int[] rle = new int[LITLEN + DIST];
    private final int[] leaves = new int[LITLEN], weight = new int[2 * LITLEN], parent = new int[2 * LITLEN];
    private final int[] depthCount = new int[2 * LITLEN], nextCode = new int[MAX_BITS + 2];

    private long bits;
    private int nbits, pos;

    Gzip(int capacity) {
        in = new byte[capacity + 8];
        tokens = new int[capacity];
        buf = new byte[capacity + capacity / 8 + 1024];
    }

    void begin() { size = 0; }

    void add(byte[] a, int off, int n) {
        if (size + n > tokens.length) throw new IllegalStateException("gzip buffer full");
        System.arraycopy(a, off, in, size, n);
        size += n;
    }

    int end() {
        crc.reset();
        crc.update(in, 0, size);
        System.arraycopy(HEADER, 0, buf, 0, HEADER.length);
        pos = HEADER.length;
        bits = 0;
        nbits = 0;
        block(tokenize());
        align();
        I32.set(buf, pos, (int) crc.getValue());
        I32.set(buf, pos + 4, size);
        return pos + 8;
    }

    private static int hash(int x) { return (x * 0x1E35A7BD) >>> (32 - HASH_BITS); }

    private static int distSymbol(int dist) {
        int d = dist - 1;
        return DIST_CODE[d < 256 ? d : 256 + (d >>> 7)];
    }

    private int tokenize() {
        byte[] b = in;
        int n = size, nt = 0, i = 0, last = n - MIN_MATCH;
        int[] h = head, t = tokens, lf = litFreq, df = distFreq;
        Arrays.fill(lf, 0);
        Arrays.fill(df, 0);
        if (base > Integer.MAX_VALUE - n) {
            Arrays.fill(h, 0);
            base = 1;
        }
        int bs = base;
        while (i <= last) {
            int x = (int) I32.get(b, i), k = hash(x), cand = h[k] - bs;
            h[k] = i + bs;
            if (cand < 0 || i - cand > WINDOW || (int) I32.get(b, cand) != x) {
                int c = b[i++] & 0xff;
                t[nt++] = c;
                lf[c]++;
                continue;
            }
            int max = Math.min(MAX_MATCH, n - i), len = MIN_MATCH;
            while (len < max) {
                long d = (long) I64.get(b, cand + len) ^ (long) I64.get(b, i + len);
                if (d != 0) {
                    len += Long.numberOfTrailingZeros(d) >>> 3;
                    break;
                }
                len += 8;
            }
            if (len > max) len = max;
            int dist = i - cand;
            t[nt++] = len << 16 | dist;
            lf[EOB + 1 + LEN_CODE[len]]++;
            df[distSymbol(dist)]++;
            int end = i + len;
            for (int p = i + 1, to = Math.min(end, last + 1); p < to; p++) h[hash((int) I32.get(b, p))] = p + bs;
            i = end;
        }
        while (i < n) {
            int c = b[i++] & 0xff;
            t[nt++] = c;
            lf[c]++;
        }
        lf[EOB] = 1;
        base = bs + n;
        return nt;
    }

    private void block(int nt) {
        lengths(litFreq, LITLEN, MAX_BITS, litLen);
        lengths(distFreq, DIST, MAX_BITS, distLen);
        int hlit = LITLEN, hdist = DIST;
        while (hlit > 257 && litLen[hlit - 1] == 0) hlit--;
        while (hdist > 1 && distLen[hdist - 1] == 0) hdist--;
        System.arraycopy(litLen, 0, lens, 0, hlit);
        System.arraycopy(distLen, 0, lens, hlit, hdist);
        int nr = runs(hlit + hdist);
        lengths(clFreq, CODELEN, MAX_CL_BITS, clLen);
        int hclen = CODELEN;
        while (hclen > 4 && clLen[CODELEN_ORDER[hclen - 1]] == 0) hclen--;

        long extra = 0, dynamic = 17 + 3L * hclen, fixed = 3;
        for (int c = 0; c < LEN_BASE.length; c++) extra += (long) litFreq[EOB + 1 + c] * LEN_EXTRA[c];
        for (int c = 0; c < DIST; c++) extra += (long) distFreq[c] * DIST_EXTRA[c];
        for (int s = 0; s < LITLEN; s++) {
            dynamic += (long) litFreq[s] * litLen[s];
            fixed += (long) litFreq[s] * FIXED_LIT_LEN[s];
        }
        for (int c = 0; c < DIST; c++) {
            dynamic += (long) distFreq[c] * distLen[c];
            fixed += (long) distFreq[c] * FIXED_DIST_LEN[c];
        }
        for (int s = 0; s < CODELEN; s++) {
            dynamic += (long) clFreq[s] * (clLen[s] + (s >= 16 ? CODELEN_EXTRA[s - 16] : 0));
        }
        long stored = 8L * (size + 5L * Math.max(1, (size + 65534) / 65535));
        dynamic += extra;
        fixed += extra;

        if (stored < dynamic && stored < fixed) {
            stored();
        } else if (fixed <= dynamic) {
            put(1 | 1 << 1, 3);
            data(nt, FIXED_LIT_CODE, FIXED_LIT_LEN, FIXED_DIST_CODE, FIXED_DIST_LEN);
        } else {
            codes(litLen, LITLEN, litCode, nextCode);
            codes(distLen, DIST, distCode, nextCode);
            codes(clLen, CODELEN, clCode, nextCode);
            put(1 | 2 << 1, 3);
            put(hlit - 257, 5);
            put(hdist - 1, 5);
            put(hclen - 4, 4);
            for (int k = 0; k < hclen; k++) put(clLen[CODELEN_ORDER[k]], 3);
            for (int k = 0; k < nr; k++) {
                int s = rle[k] & 31;
                put(clCode[s], clLen[s]);
                if (s >= 16) put(rle[k] >>> 5, CODELEN_EXTRA[s - 16]);
            }
            data(nt, litCode, litLen, distCode, distLen);
        }
    }

    private int runs(int total) {
        Arrays.fill(clFreq, 0);
        int nr = 0, k = 0;
        while (k < total) {
            int v = lens[k], run = 1;
            while (k + run < total && lens[k + run] == v) run++;
            k += run;
            if (v == 0) {
                while (run >= 11) {
                    int r = Math.min(run, 138);
                    nr = run(nr, 18, r - 11);
                    run -= r;
                }
                if (run >= 3) {
                    nr = run(nr, 17, run - 3);
                    run = 0;
                }
            } else {
                nr = run(nr, v, 0);
                run--;
                while (run >= 3) {
                    int r = Math.min(run, 6);
                    nr = run(nr, 16, r - 3);
                    run -= r;
                }
            }
            while (run-- > 0) nr = run(nr, v, 0);
        }
        return nr;
    }

    private int run(int nr, int symbol, int extra) {
        rle[nr] = symbol | extra << 5;
        clFreq[symbol]++;
        return nr + 1;
    }

    private void data(int nt, int[] lc, byte[] ll, int[] dc, byte[] dl) {
        int[] t = tokens;
        byte[] o = buf;
        long bb = bits;
        int nb = nbits, p = pos;
        for (int k = 0; k < nt; k++) {
            int v = t[k];
            if (v < 256) {
                bb |= (long) lc[v] << nb;
                nb += ll[v];
            } else {
                int len = v >>> 16, dist = v & 0xffff, c = LEN_CODE[len], s = EOB + 1 + c, d = distSymbol(dist);
                bb |= (long) (lc[s] | (len - LEN_BASE[c]) << ll[s]) << nb;
                nb += ll[s] + LEN_EXTRA[c];
                if (nb >= 32) {
                    I32.set(o, p, (int) bb);
                    p += 4;
                    bb >>>= 32;
                    nb -= 32;
                }
                bb |= (long) (dc[d] | (dist - DIST_BASE[d]) << dl[d]) << nb;
                nb += dl[d] + DIST_EXTRA[d];
            }
            if (nb >= 32) {
                I32.set(o, p, (int) bb);
                p += 4;
                bb >>>= 32;
                nb -= 32;
            }
        }
        bits = bb;
        nbits = nb;
        pos = p;
        put(lc[EOB], ll[EOB]);
    }

    private void stored() {
        int from = 0;
        do {
            int n = Math.min(size - from, 65535);
            put(from + n == size ? 1 : 0, 3);
            align();
            buf[pos] = (byte) n;
            buf[pos + 1] = (byte) (n >>> 8);
            buf[pos + 2] = (byte) ~n;
            buf[pos + 3] = (byte) (~n >>> 8);
            System.arraycopy(in, from, buf, pos + 4, n);
            pos += 4 + n;
            from += n;
        } while (from < size);
    }

    private void put(int value, int n) {
        bits |= (long) value << nbits;
        nbits += n;
        if (nbits >= 32) {
            I32.set(buf, pos, (int) bits);
            pos += 4;
            bits >>>= 32;
            nbits -= 32;
        }
    }

    private void align() {
        while (nbits > 0) {
            buf[pos++] = (byte) bits;
            bits >>>= 8;
            nbits -= 8;
        }
        bits = 0;
        nbits = 0;
    }

    private void lengths(int[] freq, int n, int limit, byte[] len) {
        int m = 0;
        for (int s = 0; s < n; s++) {
            len[s] = 0;
            if (freq[s] > 0) leaves[m++] = freq[s] << 9 | s;
        }
        if (m < 2) {
            int s = m == 1 ? leaves[0] & 511 : 0;
            len[s] = 1;
            len[s == 0 ? 1 : 0] = 1;
            return;
        }
        Arrays.sort(leaves, 0, m);
        for (int k = 0; k < m; k++) weight[k] = leaves[k] >>> 9;
        int leaf = 0, node = m, next = m;
        for (int k = 0; k < m - 1; k++, next++) {
            int a = leaf < m && (node >= next || weight[leaf] <= weight[node]) ? leaf++ : node++;
            int b = leaf < m && (node >= next || weight[leaf] <= weight[node]) ? leaf++ : node++;
            weight[next] = weight[a] + weight[b];
            parent[a] = parent[b] = next;
        }
        int root = next - 1, deepest = 0;
        weight[root] = 0;
        for (int k = root - 1; k >= 0; k--) {
            weight[k] = weight[parent[k]] + 1;
            if (k < m) {
                depthCount[weight[k]]++;
                deepest = Math.max(deepest, weight[k]);
            }
        }
        for (int d = deepest; d > limit; d--) {
            while (depthCount[d] > 0) {
                int j = d - 2;
                while (depthCount[j] == 0) j--;
                depthCount[d] -= 2;
                depthCount[d - 1]++;
                depthCount[j + 1] += 2;
                depthCount[j]--;
            }
        }
        int k = m - 1;
        for (int d = 1; d <= Math.min(deepest, limit); d++) {
            for (int c = depthCount[d]; c > 0; c--) len[leaves[k--] & 511] = (byte) d;
            depthCount[d] = 0;
        }
    }

    private static void codes(byte[] len, int n, int[] code, int[] next) {
        Arrays.fill(next, 0);
        for (int s = 0; s < n; s++) next[len[s]]++;
        next[0] = 0;
        for (int b = 1, c = 0; b <= MAX_BITS; b++) {
            int count = next[b];
            next[b] = c;
            c = (c + count) << 1;
        }
        for (int s = 0; s < n; s++) {
            int l = len[s];
            if (l != 0) code[s] = Integer.reverse(next[l]++) >>> (32 - l);
        }
    }
}
