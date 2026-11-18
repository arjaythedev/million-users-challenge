// SHA-256, HMAC-SHA-256, PBKDF2 (for SCRAM) and base64. No dependencies.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; uint32_t n; } sha256_t;

static const uint32_t SHA_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        uint32_t v;
        memcpy(&v, p + 4 * i, 4);
        w[i] = __builtin_bswap32(v);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + SHA_K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

static void sha256_init(sha256_t *s) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof iv);
    s->len = 0;
    s->n = 0;
}

static void sha256_update(sha256_t *s, const void *data, size_t len) {
    const uint8_t *p = data;
    s->len += len;
    if (s->n) {
        size_t take = 64 - s->n;
        if (take > len) take = len;
        memcpy(s->buf + s->n, p, take);
        s->n += take; p += take; len -= take;
        if (s->n < 64) return;
        sha256_block(s->h, s->buf);
        s->n = 0;
    }
    while (len >= 64) { sha256_block(s->h, p); p += 64; len -= 64; }
    if (len) { memcpy(s->buf, p, len); s->n = len; }
}

static void sha256_final(sha256_t *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;
    uint8_t pad[72];
    size_t padlen = (s->n < 56) ? 56 - s->n : 120 - s->n;
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    for (int i = 0; i < 8; i++) pad[padlen + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(s, pad, padlen + 8);
    for (int i = 0; i < 8; i++) {
        uint32_t v = __builtin_bswap32(s->h[i]);
        memcpy(out + 4 * i, &v, 4);
    }
}

// HMAC with the key's inner and outer pad states computed once.
typedef struct { sha256_t in, out; } hmac_key_t;

static void hmac_key_init(hmac_key_t *k, const void *key, size_t klen) {
    uint8_t kb[64], x[64];
    memset(kb, 0, 64);
    if (klen > 64) {
        sha256_t s; sha256_init(&s); sha256_update(&s, key, klen); sha256_final(&s, kb);
    } else memcpy(kb, key, klen);
    for (int i = 0; i < 64; i++) x[i] = kb[i] ^ 0x36;
    sha256_init(&k->in); sha256_update(&k->in, x, 64);
    for (int i = 0; i < 64; i++) x[i] = kb[i] ^ 0x5c;
    sha256_init(&k->out); sha256_update(&k->out, x, 64);
}

static void hmac_sha256(const hmac_key_t *k, const void *msg, size_t len, uint8_t mac[32]) {
    sha256_t s = k->in;
    uint8_t inner[32];
    sha256_update(&s, msg, len);
    sha256_final(&s, inner);
    s = k->out;
    sha256_update(&s, inner, 32);
    sha256_final(&s, mac);
}

// PBKDF2-HMAC-SHA256 with a 32-byte output: SCRAM's Hi().
static void pbkdf2_sha256(const void *pw, size_t pwlen, const uint8_t *salt, size_t saltlen, uint32_t iters, uint8_t out[32]) {
    hmac_key_t k;
    hmac_key_init(&k, pw, pwlen);
    uint8_t u[32], msg[256];
    if (saltlen > 240) saltlen = 240;
    memcpy(msg, salt, saltlen);
    msg[saltlen] = 0; msg[saltlen + 1] = 0; msg[saltlen + 2] = 0; msg[saltlen + 3] = 1;
    hmac_sha256(&k, msg, saltlen + 4, u);
    memcpy(out, u, 32);
    for (uint32_t i = 1; i < iters; i++) {
        hmac_sha256(&k, u, 32, u);
        for (int j = 0; j < 32; j++) out[j] ^= u[j];
    }
}

// ---- base64 ----
static const char B64STD[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const uint8_t *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = in[i] << 16;
        if (i + 1 < n) v |= in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = B64STD[v >> 18];
        out[o++] = B64STD[(v >> 12) & 63];
        out[o++] = i + 1 < n ? B64STD[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? B64STD[v & 63] : '=';
    }
    return o;
}

static int8_t B64DEC[256];      // standard and URL alphabets, -1 for anything else
static void b64_init(void) {
    memset(B64DEC, -1, sizeof B64DEC);
    for (int i = 0; i < 64; i++) B64DEC[(uint8_t)B64STD[i]] = (int8_t)i;
    B64DEC['-'] = 62;
    B64DEC['_'] = 63;
}

// Decodes unpadded or padded base64/base64url. Returns the decoded length, or -1 if a character is invalid
// or the length is impossible. `out` needs room for n * 3 / 4 bytes.
static long b64_decode(const char *in, size_t n, uint8_t *out) {
    while (n && in[n - 1] == '=') n--;
    if ((n & 3) == 1) return -1;
    size_t o = 0, i = 0;
    for (; i + 4 <= n; i += 4) {
        int a = B64DEC[(uint8_t)in[i]], b = B64DEC[(uint8_t)in[i + 1]], c = B64DEC[(uint8_t)in[i + 2]], d = B64DEC[(uint8_t)in[i + 3]];
        if ((a | b | c | d) < 0) return -1;
        uint32_t v = (uint32_t)a << 18 | (uint32_t)b << 12 | (uint32_t)c << 6 | (uint32_t)d;
        out[o++] = (uint8_t)(v >> 16); out[o++] = (uint8_t)(v >> 8); out[o++] = (uint8_t)v;
    }
    if (n - i == 2) {
        int a = B64DEC[(uint8_t)in[i]], b = B64DEC[(uint8_t)in[i + 1]];
        if ((a | b) < 0) return -1;
        out[o++] = (uint8_t)((a << 2) | (b >> 4));
    } else if (n - i == 3) {
        int a = B64DEC[(uint8_t)in[i]], b = B64DEC[(uint8_t)in[i + 1]], c = B64DEC[(uint8_t)in[i + 2]];
        if ((a | b | c) < 0) return -1;
        out[o++] = (uint8_t)((a << 2) | (b >> 4));
        out[o++] = (uint8_t)((b << 4) | (c >> 2));
    }
    return (long)o;
}
