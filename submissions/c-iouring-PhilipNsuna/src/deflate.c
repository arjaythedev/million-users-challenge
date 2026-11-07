#include "deflate.h"
#include <stdlib.h>
#include <string.h>

static const uint16_t length_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const uint8_t cl_order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
static uint8_t length_code[256];
static uint32_t crc_table[8][256];

static void tables_init(void) {
    static int done;
    if (done) return;
    for (int c = 0; c < 29; c++) {
        int n = length_base[c], hi = c == 28 ? n + 1 : n + (1 << length_extra[c]);
        for (int l = n; l < hi && l <= 258; l++) length_code[l - 3] = (uint8_t)c;
    }
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        crc_table[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++)
        for (int k = 1; k < 8; k++) crc_table[k][i] = crc_table[0][crc_table[k - 1][i] & 0xFF] ^ (crc_table[k - 1][i] >> 8);
    done = 1;
}

uint32_t crc32_ieee(const uint8_t *p, size_t n) { // slicing-by-8
    tables_init();
    uint32_t c = 0xFFFFFFFF;
    while (n >= 8) {
        uint32_t one, two;
        memcpy(&one, p, 4); memcpy(&two, p + 4, 4);
        one ^= c;
        c = crc_table[7][one & 0xFF] ^ crc_table[6][one >> 8 & 0xFF] ^ crc_table[5][one >> 16 & 0xFF] ^ crc_table[4][one >> 24] ^
            crc_table[3][two & 0xFF] ^ crc_table[2][two >> 8 & 0xFF] ^ crc_table[1][two >> 16 & 0xFF] ^ crc_table[0][two >> 24];
        p += 8; n -= 8;
    }
    while (n--) c = crc_table[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static int dist_code(int d) {
    uint32_t x = (uint32_t)(d - 1);
    if (x < 4) return (int)x;
    int msb = 31 - __builtin_clz(x);
    return 2 * msb + (int)(x >> (msb - 1) & 1);
}

static inline uint32_t ld32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t ld64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint32_t hash4(const uint8_t *p) { return (ld32(p) * 0x1E35A7BDu) >> (32 - DF_HASH_BITS); }

// --- Huffman code construction (init time only) ---
static void plain_huffman(const int *freq, int n, uint8_t *lengths) {
    int w[600], left[600], right[600], sym[600];
    int live[300], nlive = n, nn = n;
    for (int i = 0; i < n; i++) { w[i] = freq[i]; sym[i] = i; left[i] = right[i] = -1; live[i] = i; }
    while (nlive > 1) {
        // pick the two lightest (stable: lower index first on ties)
        int a = -1, b = -1;
        for (int i = 0; i < nlive; i++) {
            int x = live[i];
            if (a < 0 || w[x] < w[a]) { b = a; a = x; } else if (b < 0 || w[x] < w[b]) b = x;
        }
        w[nn] = w[a] + w[b]; sym[nn] = -1; left[nn] = a; right[nn] = b;
        int k = 0;
        for (int i = 0; i < nlive; i++) if (live[i] != a && live[i] != b) live[k++] = live[i];
        live[k++] = nn; nlive = k; nn++;
    }
    // depths via explicit stack
    int stack[600], depth[600], sp = 0;
    stack[sp] = live[0]; depth[sp] = 0; sp++;
    while (sp) {
        sp--; int x = stack[sp], d = depth[sp];
        if (sym[x] >= 0) { lengths[sym[x]] = (uint8_t)(d ? d : 1); continue; }
        stack[sp] = left[x]; depth[sp] = d + 1; sp++;
        stack[sp] = right[x]; depth[sp] = d + 1; sp++;
    }
}

static void huffman_lengths(const int *freq0, int n, int maxbits, uint8_t *lengths) {
    int freq[300];
    memcpy(freq, freq0, (size_t)n * sizeof *freq);
    for (;;) {
        plain_huffman(freq, n, lengths);
        int ok = 1;
        for (int i = 0; i < n; i++) if (lengths[i] > maxbits) { ok = 0; break; }
        if (ok) return;
        for (int i = 0; i < n; i++) freq[i] = freq[i] / 2 > 1 ? freq[i] / 2 : 1;
    }
}

static uint32_t reverse_bits(uint32_t c, int n) { uint32_t r = 0; for (int i = 0; i < n; i++) { r = r << 1 | (c & 1); c >>= 1; } return r; }

static void canonical(const uint8_t *lengths, int n, dfcode_t *codes) {
    int bl[16] = {0};
    for (int i = 0; i < n; i++) bl[lengths[i]]++;
    bl[0] = 0;
    uint32_t next[16], code = 0;
    for (int bits = 1; bits <= 15; bits++) { code = (code + (uint32_t)bl[bits - 1]) << 1; next[bits] = code; }
    for (int i = 0; i < n; i++) {
        if (!lengths[i]) { codes[i].bits = 0; codes[i].n = 0; continue; }
        codes[i].bits = reverse_bits(next[lengths[i]]++, lengths[i]);
        codes[i].n = lengths[i];
    }
}

typedef struct { uint64_t acc; unsigned nacc; buf_t *out; } bitw_t;
static void put(bitw_t *w, uint32_t bits, unsigned n) {
    w->acc |= (uint64_t)bits << w->nacc; w->nacc += n;
    if (w->nacc >= 32) { uint32_t v = (uint32_t)w->acc; buf_append(w->out, &v, 4); w->acc >>= 32; w->nacc -= 32; }
}
static void flush_bits(bitw_t *w) {
    while (w->nacc > 0) { uint8_t b = (uint8_t)w->acc; buf_append(w->out, &b, 1); w->acc >>= 8; w->nacc = w->nacc >= 8 ? w->nacc - 8 : 0; }
}

static void build_header(deflater_t *d, const uint8_t *litlen, const uint8_t *distlen) {
    int hlit = 286; while (hlit > 257 && !litlen[hlit - 1]) hlit--;
    int hdist = 30; while (hdist > 1 && !distlen[hdist - 1]) hdist--;
    uint8_t all[316]; int na = 0;
    for (int i = 0; i < hlit; i++) all[na++] = litlen[i];
    for (int i = 0; i < hdist; i++) all[na++] = distlen[i];
    struct { int sym, extra, nextra; } syms[320]; int ns = 0;
    for (int i = 0; i < na;) {
        uint8_t l = all[i]; int run = 1;
        while (i + run < na && all[i + run] == l) run++;
        if (l == 0 && run >= 3) {
            int r = run > 138 ? 138 : run;
            if (r <= 10) { syms[ns].sym = 17; syms[ns].extra = r - 3; syms[ns].nextra = 3; }
            else { syms[ns].sym = 18; syms[ns].extra = r - 11; syms[ns].nextra = 7; }
            ns++; i += r; continue;
        }
        syms[ns].sym = l; syms[ns].extra = 0; syms[ns].nextra = 0; ns++;
        i++; run--;
        while (run >= 3) { int r = run > 6 ? 6 : run; syms[ns].sym = 16; syms[ns].extra = r - 3; syms[ns].nextra = 2; ns++; i += r; run -= r; }
    }
    int clfreq[19]; for (int i = 0; i < 19; i++) clfreq[i] = 1;
    for (int i = 0; i < ns; i++) clfreq[syms[i].sym] += 4;
    uint8_t cllen[19]; huffman_lengths(clfreq, 19, 7, cllen);
    dfcode_t clcodes[19]; canonical(cllen, 19, clcodes);
    int hclen = 19; while (hclen > 4 && !cllen[cl_order[hclen - 1]]) hclen--;
    buf_reset(&d->out);
    bitw_t w = {0, 0, &d->out};
    put(&w, 1, 1); put(&w, 2, 2);
    put(&w, (uint32_t)(hlit - 257), 5); put(&w, (uint32_t)(hdist - 1), 5); put(&w, (uint32_t)(hclen - 4), 4);
    for (int i = 0; i < hclen; i++) put(&w, cllen[cl_order[i]], 3);
    for (int i = 0; i < ns; i++) { put(&w, clcodes[syms[i].sym].bits, clcodes[syms[i].sym].n); if (syms[i].nextra) put(&w, (uint32_t)syms[i].extra, (unsigned)syms[i].nextra); }
    int hb = (int)d->out.len * 8 + (int)w.nacc;
    flush_bits(&w);
    memcpy(d->header, d->out.p, d->out.len);
    d->hbits = hb;
}

// tokenize for training: counts symbols
static void train(deflater_t *d, const uint8_t *src, size_t n, int *litfreq, int *distfreq) {
    memset(d->head, 0, sizeof d->head);
    size_t i = 0;
    while (i + 8 <= n) {
        uint32_t h = hash4(src + i);
        long cand = (long)d->head[h] - 1;
        d->head[h] = (int32_t)(i + 1);
        if (cand >= 0 && (size_t)cand < i && i - (size_t)cand <= 32768 && ld32(src + cand) == ld32(src + i)) {
            int l = 4;
            while (i + (size_t)l < n && l < 258 && src[cand + l] == src[i + l]) l++;
            litfreq[257 + length_code[l - 3]] += 8;
            distfreq[dist_code((int)(i - (size_t)cand))] += 8;
            i += (size_t)l;
            continue;
        }
        litfreq[src[i]] += 8; i++;
    }
    for (; i < n; i++) litfreq[src[i]] += 8;
}

void deflater_init(deflater_t *d, const uint8_t *sample, size_t n) {
    tables_init();
    memset(d, 0, sizeof *d);
    buf_reserve(&d->out, 16384);
    int litfreq[286], distfreq[30];
    for (int i = 0; i < 286; i++) litfreq[i] = 1;
    for (int i = 0; i < 30; i++) distfreq[i] = 1;
    train(d, sample, n, litfreq, distfreq);
    litfreq[256] = 8;
    uint8_t litlen[286], distlen[30];
    huffman_lengths(litfreq, 286, 15, litlen);
    huffman_lengths(distfreq, 30, 15, distlen);
    canonical(litlen, 286, d->lit);
    canonical(distlen, 30, d->dist);
    build_header(d, litlen, distlen);
    memset(d->head, 0, sizeof d->head);
}

const uint8_t *deflate_gzip(deflater_t *d, const uint8_t *src, size_t n, size_t *outlen) {
    buf_t *out = &d->out;
    buf_reset(out);
    buf_reserve(out, n + 64);
    static const uint8_t gzh[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
    buf_append(out, gzh, 10);
    int full = d->hbits / 8;
    buf_append(out, d->header, (size_t)full);
    bitw_t w = {0, 0, out};
    unsigned rem = (unsigned)(d->hbits % 8);
    if (rem) { w.acc = d->header[full] & ((1u << rem) - 1); w.nacc = rem; }
    const dfcode_t *lit = d->lit;
    int32_t *head = d->head;
    size_t i = 0;
    while (i + 8 <= n) {
        uint32_t v = ld32(src + i);
        uint32_t h = (v * 0x1E35A7BDu) >> (32 - DF_HASH_BITS);
        long cand = (long)head[h] - 1;
        head[h] = (int32_t)(i + 1);
        if (cand >= 0 && (size_t)cand < i && i - (size_t)cand <= 32768 && ld32(src + cand) == v) {
            int l = 4;
            for (;;) {
                if (i + (size_t)l + 8 > n || l + 8 > 258) {
                    while (i + (size_t)l < n && l < 258 && src[cand + l] == src[i + l]) l++;
                    break;
                }
                uint64_t x = ld64(src + cand + l) ^ ld64(src + i + l);
                if (x) { l += __builtin_ctzll(x) >> 3; break; }
                l += 8;
            }
            int lc = length_code[l - 3];
            put(&w, lit[257 + lc].bits, lit[257 + lc].n);
            if (length_extra[lc]) put(&w, (uint32_t)(l - length_base[lc]), length_extra[lc]);
            int dist = (int)(i - (size_t)cand);
            int dc = dist_code(dist);
            put(&w, d->dist[dc].bits, d->dist[dc].n);
            if (dist_extra[dc]) put(&w, (uint32_t)(dist - dist_base[dc]), dist_extra[dc]);
            if (i + (size_t)l + 4 <= n) { size_t j = i + (size_t)l - 2; head[hash4(src + j)] = (int32_t)(j + 1); }
            i += (size_t)l;
            continue;
        }
        put(&w, lit[src[i]].bits, lit[src[i]].n);
        i++;
    }
    for (; i < n; i++) put(&w, lit[src[i]].bits, lit[src[i]].n);
    put(&w, lit[256].bits, lit[256].n);
    flush_bits(&w);
    uint32_t crc = crc32_ieee(src, n), isize = (uint32_t)n;
    buf_append(out, &crc, 4); buf_append(out, &isize, 4);
    *outlen = out->len;
    return out->p;
}
