// gzip for the feed: one deflate block with the fixed Huffman codes, written by an encoder that knows the
// shape of the JSON it is compressing.
//
// The feed is 20 post objects with the same keys. The JSON writer records where each key run starts
// ("marks"), so the encoder never searches for those: a key run in post i is a match against the same run
// in post i-1 at a distance it already knows, extended over whatever follows and is equal too (the id's
// leading digits, the timestamp's date and hour, ...). Only the two free-text fields (body, author) go
// through a small hash matcher. Any feed compresses correctly; the layout only decides how cheaply.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <immintrin.h>

// ---- CRC-32 (IEEE, as gzip wants) ----
static uint32_t CRC_T[8][256];

static void crc_tables_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
        CRC_T[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++)
        for (int t = 1; t < 8; t++) CRC_T[t][i] = (CRC_T[t - 1][i] >> 8) ^ CRC_T[0][CRC_T[t - 1][i] & 0xff];
}

// state in, state out (no inversion here)
static uint32_t crc_table_raw(uint32_t c, const uint8_t *p, size_t n) {
    while (n >= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        v ^= c;
        c = CRC_T[7][v & 0xff] ^ CRC_T[6][(v >> 8) & 0xff] ^ CRC_T[5][(v >> 16) & 0xff] ^ CRC_T[4][(v >> 24) & 0xff] ^
            CRC_T[3][(v >> 32) & 0xff] ^ CRC_T[2][(v >> 40) & 0xff] ^ CRC_T[1][(v >> 48) & 0xff] ^ CRC_T[0][v >> 56];
        p += 8; n -= 8;
    }
    while (n--) c = (c >> 8) ^ CRC_T[0][(c ^ *p++) & 0xff];
    return c;
}

// Carry-less multiply folding (the constants of Intel's "Fast CRC computation using PCLMULQDQ" for the
// reflected IEEE polynomial). len must be >= 64 and a multiple of 16. State in, state out.
__attribute__((target("pclmul,sse4.1")))
static uint32_t crc_clmul_raw(uint32_t crc, const uint8_t *buf, size_t len) {
    const __m128i k1k2 = _mm_set_epi64x(0x01c6e41596, 0x0154442bd4);
    const __m128i k3k4 = _mm_set_epi64x(0x00ccaa009e, 0x01751997d0);
    const __m128i k5k0 = _mm_set_epi64x(0x0000000000, 0x0163cd6124);
    const __m128i poly = _mm_set_epi64x(0x01f7011641, 0x01db710641);
    __m128i x0, x1, x2, x3, x4, x5, x6, x7, x8, y5, y6, y7, y8;
    x1 = _mm_loadu_si128((const __m128i *)(buf + 0x00));
    x2 = _mm_loadu_si128((const __m128i *)(buf + 0x10));
    x3 = _mm_loadu_si128((const __m128i *)(buf + 0x20));
    x4 = _mm_loadu_si128((const __m128i *)(buf + 0x30));
    x1 = _mm_xor_si128(x1, _mm_cvtsi32_si128((int)crc));
    x0 = k1k2;
    buf += 64; len -= 64;
    while (len >= 64) {
        x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
        x6 = _mm_clmulepi64_si128(x2, x0, 0x00);
        x7 = _mm_clmulepi64_si128(x3, x0, 0x00);
        x8 = _mm_clmulepi64_si128(x4, x0, 0x00);
        x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
        x2 = _mm_clmulepi64_si128(x2, x0, 0x11);
        x3 = _mm_clmulepi64_si128(x3, x0, 0x11);
        x4 = _mm_clmulepi64_si128(x4, x0, 0x11);
        y5 = _mm_loadu_si128((const __m128i *)(buf + 0x00));
        y6 = _mm_loadu_si128((const __m128i *)(buf + 0x10));
        y7 = _mm_loadu_si128((const __m128i *)(buf + 0x20));
        y8 = _mm_loadu_si128((const __m128i *)(buf + 0x30));
        x1 = _mm_xor_si128(_mm_xor_si128(x1, x5), y5);
        x2 = _mm_xor_si128(_mm_xor_si128(x2, x6), y6);
        x3 = _mm_xor_si128(_mm_xor_si128(x3, x7), y7);
        x4 = _mm_xor_si128(_mm_xor_si128(x4, x8), y8);
        buf += 64; len -= 64;
    }
    x0 = k3k4;
    x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
    x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
    x1 = _mm_xor_si128(_mm_xor_si128(x1, x2), x5);
    x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
    x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
    x1 = _mm_xor_si128(_mm_xor_si128(x1, x3), x5);
    x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
    x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
    x1 = _mm_xor_si128(_mm_xor_si128(x1, x4), x5);
    while (len >= 16) {
        x2 = _mm_loadu_si128((const __m128i *)buf);
        x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
        x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
        x1 = _mm_xor_si128(_mm_xor_si128(x1, x2), x5);
        buf += 16; len -= 16;
    }
    x2 = _mm_clmulepi64_si128(x1, x0, 0x10);
    x3 = _mm_setr_epi32(~0, 0, ~0, 0);
    x1 = _mm_srli_si128(x1, 8);
    x1 = _mm_xor_si128(x1, x2);
    x0 = _mm_loadl_epi64(&k5k0);
    x2 = _mm_srli_si128(x1, 4);
    x1 = _mm_and_si128(x1, x3);
    x1 = _mm_clmulepi64_si128(x1, x0, 0x00);
    x1 = _mm_xor_si128(x1, x2);
    x0 = poly;
    x2 = _mm_and_si128(x1, x3);
    x2 = _mm_clmulepi64_si128(x2, x0, 0x10);
    x2 = _mm_and_si128(x2, x3);
    x2 = _mm_clmulepi64_si128(x2, x0, 0x00);
    x1 = _mm_xor_si128(x1, x2);
    return (uint32_t)_mm_extract_epi32(x1, 1);
}

static int crc_use_clmul;

static uint32_t crc32_gz(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffffu;
    if (crc_use_clmul && n >= 64) {
        size_t chunk = n & ~(size_t)15;
        c = crc_clmul_raw(c, p, chunk);
        p += chunk; n -= chunk;
    }
    return ~crc_table_raw(c, p, n);
}

// ---- fixed Huffman code tables ----
static uint16_t GZ_LIT[256];       // reversed code; 8 bits for 0..143, 9 for 144..255
static uint8_t GZ_L8[256];         // the same for bytes below 128, as one byte: four of them fill one 32-bit write
static uint32_t GZ_LEN[259];       // low 24 bits: code + extra bits, top 8: bit count
static uint8_t GZ_DCODE[30];       // reversed 5-bit distance codes
static const uint16_t GZ_DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                      1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};

static uint32_t gz_rev(uint32_t v, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; i++) { r = (r << 1) | (v & 1); v >>= 1; }
    return r;
}

static void gz_init(void) {
    static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                       67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    crc_tables_init();
    for (int b = 0; b < 256; b++) GZ_LIT[b] = (uint16_t)(b < 144 ? gz_rev(0x30 + b, 8) : gz_rev(0x190 + (b - 144), 9));
    for (int b = 0; b < 128; b++) GZ_L8[b] = (uint8_t)GZ_LIT[b];
    for (int len = 3; len <= 258; len++) {
        int i = 28;
        while (lbase[i] > len) i--;
        if (len == 258) i = 28;
        int sym = 257 + i, nb;
        uint32_t code;
        if (sym < 280) { code = gz_rev(sym - 256, 7); nb = 7; }
        else { code = gz_rev(0xC0 + (sym - 280), 8); nb = 8; }
        code |= (uint32_t)(len - lbase[i]) << nb;
        GZ_LEN[len] = code | (uint32_t)(nb + lext[i]) << 24;
    }
    for (int i = 0; i < 30; i++) GZ_DCODE[i] = (uint8_t)gz_rev(i, 5);
    // self-check of the folding CRC against the table one; any disagreement keeps the table version
    crc_use_clmul = 0;
    if (__builtin_cpu_supports("pclmul") && __builtin_cpu_supports("sse4.1")) {
        uint8_t t[1024];
        uint32_t x = 12345;
        for (int i = 0; i < 1024; i++) { x = x * 1103515245u + 12345u; t[i] = (uint8_t)(x >> 16); }
        int ok = 1;
        for (size_t n = 64; n <= 1024 && ok; n += 16)
            if (crc_clmul_raw(0xffffffffu, t, n) != crc_table_raw(0xffffffffu, t, n)) ok = 0;
        crc_use_clmul = ok;
    }
}

typedef struct { uint8_t *p; uint64_t bb; int bn; } gzw_t;

#define GZ_PUT(w, v, n) do { \
        (w).bb |= (uint64_t)(v) << (w).bn; (w).bn += (n); \
        if ((w).bn >= 32) { uint32_t _t = (uint32_t)(w).bb; memcpy((w).p, &_t, 4); (w).p += 4; (w).bb >>= 32; (w).bn -= 32; } \
    } while (0)

static inline void gz_lits(gzw_t *w, const uint8_t *s, uint32_t n) {
    while (n) {
        uint32_t x;
        if (n >= 4 && (memcpy(&x, s, 4), !(x & 0x80808080u))) {
            // four ASCII literals are four 8-bit codes: one 32-bit write, the bit position stays where it was
            uint32_t v = GZ_L8[s[0]] | (uint32_t)GZ_L8[s[1]] << 8 | (uint32_t)GZ_L8[s[2]] << 16 | (uint32_t)GZ_L8[s[3]] << 24;
            w->bb |= (uint64_t)v << w->bn;
            uint32_t t = (uint32_t)w->bb;
            memcpy(w->p, &t, 4);
            w->p += 4;
            w->bb >>= 32;
            s += 4; n -= 4;
            continue;
        }
        uint8_t b = *s++;
        n--;
        GZ_PUT(*w, GZ_LIT[b], 8 + (b >= 144));
    }
}

static inline void gz_match(gzw_t *w, uint32_t len, uint32_t dist) {
    uint32_t l = GZ_LEN[len];
    GZ_PUT(*w, l & 0xffffff, l >> 24);
    uint32_t t = dist - 1, idx, eb;
    if (t < 4) { idx = t; eb = 0; }
    else {
        uint32_t hb = 31 - (uint32_t)__builtin_clz(t);
        idx = 2 * hb + ((t >> (hb - 1)) & 1);
        eb = hb - 1;
    }
    GZ_PUT(*w, GZ_DCODE[idx] | (dist - GZ_DBASE[idx]) << 5, 5 + eb);
}

// length of the common run of a[..] and a[-dist..], starting at `have`, at most `max`
static inline uint32_t gz_extend(const uint8_t *a, uint32_t dist, uint32_t have, uint32_t max) {
    const uint8_t *b = a - dist;
    while (have + 8 <= max) {
        uint64_t x, y;
        memcpy(&x, a + have, 8);
        memcpy(&y, b + have, 8);
        if (x != y) return have + ((uint32_t)__builtin_ctzll(x ^ y) >> 3);
        have += 8;
    }
    while (have < max && a[have] == b[have]) have++;
    return have;
}

#define GZ_HBITS 12
#define FEED_MAX_POSTS 32
// Chunk j of post i starts at k[i][j]:  j=0 `},{"id":` (for post 0: `{"posts":[{"id":`), 1 `,"body":"`,
// 2 `","created_at":"`, 3 `","author":"`, 4 `","like_count":`. Fields 1 (body) and 3 (author) are free text.
static const uint8_t FEED_KL[5] = {8, 9, 16, 12, 15};

// Writes a complete gzip member for js[0..n) to out; returns its length. n must be < 65536 and out must have
// n + n/8 + 64 bytes.
static size_t feed_gzip(const uint8_t *js, uint32_t n, uint32_t (*k)[5], int npost, uint8_t *out) {
    static uint16_t ht[1 << GZ_HBITS];
    static const uint8_t hdr[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
    memcpy(out, hdr, 10);
    gzw_t w = {out + 10, 0, 0};
    GZ_PUT(w, 3, 3);           // final block, fixed Huffman
    memset(ht, 0, sizeof ht);
    uint32_t pos = 0;
    for (int i = 0; i < npost; i++) {
        for (int j = 0; j < 5; j++) {
            uint32_t ks = k[i][j], fs = ks + ((i == 0 && j == 0) ? 16u : FEED_KL[j]);
            uint32_t fe = j < 4 ? k[i][j + 1] : (i + 1 < npost ? k[i + 1][0] : n);
            if (pos < fs) {    // the key run, or what an earlier match left of it
                uint32_t dist = i > 0 ? ks - k[i - 1][j] : 0;
                if (i == 1 && j == 0) {
                    // post 0 starts `{"posts":[{"id":`, not `},{"id":`: only the last six bytes are the same
                    if (pos < ks + 2) { gz_lits(&w, js + pos, ks + 2 - pos); pos = ks + 2; }
                    dist = ks + 2 - 10;
                }
                if (i > 0 && fs - pos >= 3 && dist <= 32768) {
                    uint32_t max = n - pos < 258 ? n - pos : 258;
                    uint32_t len = gz_extend(js + pos, dist, fs - pos, max);
                    gz_match(&w, len, dist);
                    pos += len;
                } else {
                    gz_lits(&w, js + pos, fs - pos);
                    pos = fs;
                }
            }
            if (pos >= fe) continue;
            if (j == 1 || j == 3) {
                while (pos < fe) {
                    if (pos + 4 <= n) {
                        uint32_t v;
                        memcpy(&v, js + pos, 4);
                        uint32_t h = (v * 2654435761u) >> (32 - GZ_HBITS);
                        uint32_t cand = ht[h];
                        ht[h] = (uint16_t)(pos + 1);
                        if (cand) {
                            uint32_t c = cand - 1, cv;
                            memcpy(&cv, js + c, 4);
                            if (cv == v && pos - c <= 32768) {
                                uint32_t max = n - pos < 258 ? n - pos : 258;
                                uint32_t len = gz_extend(js + pos, pos - c, 4, max);
                                gz_match(&w, len, pos - c);
                                pos += len;
                                continue;
                            }
                        }
                    }
                    uint8_t b = js[pos++];
                    GZ_PUT(w, GZ_LIT[b], 8 + (b >= 144));
                }
            } else {
                gz_lits(&w, js + pos, fe - pos);
                pos = fe;
            }
        }
    }
    if (pos < n) gz_lits(&w, js + pos, n - pos);
    GZ_PUT(w, 0, 7);           // end of block
    while (w.bn > 0) { *w.p++ = (uint8_t)w.bb; w.bb >>= 8; w.bn -= 8; }
    uint32_t crc = crc32_gz(js, n);
    memcpy(w.p, &crc, 4);
    memcpy(w.p + 4, &n, 4);
    return (size_t)(w.p + 8 - out);
}
