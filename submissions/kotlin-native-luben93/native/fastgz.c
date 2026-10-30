/*
 * gzip for small responses: greedy LZ77 over a 4-byte hash plus the fixed Huffman code, one deflate block.
 * Made for inputs of a few KB, where zlib and libdeflate spend most of their time on per-call setup, tree building and
 * lazy matching. The hash table is never cleared: entries carry a per-call base, and an entry below the current base is
 * stale. Output is plain RFC 1952 / RFC 1951, so any gzip client decodes it.
 */
#include "fastgz.h"
#include <stdlib.h>
#include <string.h>

#define HBITS 13
#define HSIZE (1u << HBITS)
#define MAXIN 65535u

struct fastgz {
    uint32_t base; /* entries >= base were written during the current call */
    uint32_t tab[HSIZE];
};

static uint32_t crc_tab[8][256];
static uint16_t lit_code[288];  /* fixed Huffman code, bit-reversed so it can be OR-ed into an LSB-first stream */
static uint8_t lit_len[288];
static uint16_t len_val[259];   /* for match length L: length code | extra bits << code length */
static uint8_t len_nbits[259];
static uint8_t drev[30];        /* 5-bit fixed distance codes, bit-reversed */
static int inited;

static const uint16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};

static uint32_t rev(uint32_t v, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; i++) { r = (r << 1) | (v & 1); v >>= 1; }
    return r;
}

static void init_tables(void) {
    for (int i = 0; i < 256; i++) {
        uint32_t c = (uint32_t)i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        crc_tab[0][i] = c;
    }
    for (int i = 0; i < 256; i++)
        for (int k = 1; k < 8; k++) crc_tab[k][i] = (crc_tab[k - 1][i] >> 8) ^ crc_tab[0][crc_tab[k - 1][i] & 255];
    for (int s = 0; s < 288; s++) {
        uint32_t code; int n;
        if (s < 144) { code = 0x30 + s; n = 8; }
        else if (s < 256) { code = 0x190 + (s - 144); n = 9; }
        else if (s < 280) { code = s - 256; n = 7; }
        else { code = 0xC0 + (s - 280); n = 8; }
        lit_code[s] = (uint16_t)rev(code, n);
        lit_len[s] = (uint8_t)n;
    }
    for (int L = 3; L <= 258; L++) {
        int k = 28;
        while (len_base[k] > L) k--;
        int sym = 257 + k;
        uint32_t extra = (uint32_t)(L - len_base[k]);
        len_val[L] = (uint16_t)(lit_code[sym] | (extra << lit_len[sym]));
        len_nbits[L] = (uint8_t)(lit_len[sym] + len_extra[k]);
    }
    for (int d = 0; d < 30; d++) drev[d] = (uint8_t)rev((uint32_t)d, 5);
    inited = 1;
}

static inline uint32_t load32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t load64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint32_t hash4(uint32_t v) { return (v * 2654435761u) >> (32 - HBITS); }

static uint32_t crc32_sb8(const uint8_t *p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    while (n >= 8) {
        uint32_t a = load32(p) ^ crc, b = load32(p + 4);
        crc = crc_tab[7][a & 255] ^ crc_tab[6][(a >> 8) & 255] ^ crc_tab[5][(a >> 16) & 255] ^ crc_tab[4][a >> 24] ^
              crc_tab[3][b & 255] ^ crc_tab[2][(b >> 8) & 255] ^ crc_tab[1][(b >> 16) & 255] ^ crc_tab[0][b >> 24];
        p += 8; n -= 8;
    }
    while (n--) crc = crc_tab[0][(crc ^ *p++) & 255] ^ (crc >> 8);
    return ~crc;
}

fastgz_t *fastgz_new(void) {
    if (!inited) init_tables();
    fastgz_t *c = (fastgz_t *)calloc(1, sizeof *c);
    if (c) c->base = 1;
    return c;
}
void fastgz_free(fastgz_t *c) { free(c); }
size_t fastgz_bound(size_t n) { return n + (n >> 3) + 64; }

/* number of equal bytes at a and b, at most max (a and b may overlap: a is ahead of b) */
static inline size_t match_len(const uint8_t *a, const uint8_t *b, size_t max, size_t start) {
    size_t l = start;
    while (l + 8 <= max) {
        uint64_t x = load64(a + l) ^ load64(b + l);
        if (x) return l + ((size_t)__builtin_ctzll(x) >> 3);
        l += 8;
    }
    while (l < max && a[l] == b[l]) l++;
    return l;
}

#define PUT(code, nbits) do { acc |= (uint64_t)(code) << nacc; nacc += (nbits); \
    if (nacc >= 32) { uint32_t w_ = (uint32_t)acc; memcpy(o, &w_, 4); o += 4; acc >>= 32; nacc -= 32; } } while (0)

size_t fastgz_compress(fastgz_t *c, const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    if (n > MAXIN || cap < fastgz_bound(n)) return 0;
    uint8_t *o = out;
    static const uint8_t hdr[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
    memcpy(o, hdr, 10); o += 10;
    uint64_t acc = 0; int nacc = 0;
    PUT(3, 3); /* BFINAL = 1, BTYPE = 01 (fixed Huffman), LSB first */
    uint32_t *tab = c->tab, base = c->base;
    size_t i = 0;
    while (i + 4 <= n) {
        uint32_t v = load32(in + i), h = hash4(v), cand = tab[h];
        tab[h] = base + (uint32_t)i;
        if (cand >= base) {
            uint32_t j = cand - base, dist = (uint32_t)i - j;
            if (dist <= 32768u && load32(in + j) == v) {
                size_t max = n - i; if (max > 258) max = 258;
                size_t l = match_len(in + i, in + j, max, 4);
                PUT(len_val[l], len_nbits[l]);
                uint32_t d1 = dist - 1, dc, ex, exn;
                if (d1 < 4) { dc = d1; ex = 0; exn = 0; }
                else { int lg = 31 - __builtin_clz(d1); dc = (uint32_t)(2 * lg) + ((d1 >> (lg - 1)) & 1); exn = (uint32_t)(lg - 1); ex = d1 & ((1u << exn) - 1); }
                PUT(drev[dc] | (ex << 5), 5 + exn);
                for (size_t k = 1; k <= 2 && k < l && i + k + 4 <= n; k++) tab[hash4(load32(in + i + k))] = base + (uint32_t)(i + k);
                i += l;
                continue;
            }
        }
        PUT(lit_code[in[i]], lit_len[in[i]]);
        i++;
    }
    for (; i < n; i++) PUT(lit_code[in[i]], lit_len[in[i]]);
    PUT(0, 7); /* end of block */
    while (nacc > 0) { *o++ = (uint8_t)acc; acc >>= 8; nacc -= 8; }
    uint32_t crc = crc32_sb8(in, n), isz = (uint32_t)n;
    memcpy(o, &crc, 4); o += 4;
    memcpy(o, &isz, 4); o += 4;
    c->base = base + (uint32_t)n + 1;
    if (c->base > 0xFFFE0000u) { memset(c->tab, 0, sizeof c->tab); c->base = 1; }
    return (size_t)(o - out);
}
