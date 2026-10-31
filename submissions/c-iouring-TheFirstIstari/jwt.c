#include "jwt.h"
#include "json.h"

#include <limits.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <string.h>
#include <time.h>

#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

/* 1s-tick-cached wall clock owned by the worker loop (net.c). */
time_t net_now_wall(void);

static _Thread_local const char *t_sec = 0;
static _Thread_local size_t t_seclen = 0;

/* Pass 9: 256-entry reverse table (single lookup per char, no branches).
 * '=' stays -1: padding is rejected exactly as before. Fully explicit to
 * keep -Woverride-init silent. */
static const signed char B64V[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, 63,
    -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};

/* base64url without padding. 0 ok, -1 invalid (bad chars, bad padding).
 * Single pass: each char is looked up exactly once (the old code looked
 * every char up twice: once to validate, once to decode). */
static int b64url_dec(const char *s, size_t n, unsigned char *out, size_t cap, size_t *olen) {
    if (n % 4 == 1) return -1;
    size_t o = 0;
    size_t full = n / 4, rem = n % 4;
    for (size_t g = 0; g < full; g++) {
        int a = B64V[(unsigned char)s[g * 4]];
        int b = B64V[(unsigned char)s[g * 4 + 1]];
        int c2 = B64V[(unsigned char)s[g * 4 + 2]];
        int d = B64V[(unsigned char)s[g * 4 + 3]];
        if (unlikely(a < 0 || b < 0 || c2 < 0 || d < 0)) return -1;
        if (unlikely(o + 3 > cap)) return -1;
        out[o++] = (unsigned char)((a << 2) | (b >> 4));
        out[o++] = (unsigned char)(((b & 15) << 4) | (c2 >> 2));
        out[o++] = (unsigned char)(((c2 & 3) << 6) | d);
    }
    if (rem == 2) {
        int a = B64V[(unsigned char)s[full * 4]];
        int b = B64V[(unsigned char)s[full * 4 + 1]];
        if (unlikely(a < 0 || b < 0)) return -1;
        if (unlikely(o + 1 > cap)) return -1;
        out[o++] = (unsigned char)((a << 2) | (b >> 4));
    } else if (rem == 3) {
        int a = B64V[(unsigned char)s[full * 4]];
        int b = B64V[(unsigned char)s[full * 4 + 1]];
        int c2 = B64V[(unsigned char)s[full * 4 + 2]];
        if (unlikely(a < 0 || b < 0 || c2 < 0)) return -1;
        if (unlikely(o + 2 > cap)) return -1;
        out[o++] = (unsigned char)((a << 2) | (b >> 4));
        out[o++] = (unsigned char)(((b & 15) << 4) | (c2 >> 2));
    }
    *olen = o;
    return 0;
}

int jwt_verify(const char *tok, size_t tlen, const char *secret,
               char *sub_out, size_t sub_cap, long long *uid_out,
               char *user_out, size_t user_cap) {
    if (!tok || tlen == 0 || !secret || !sub_out || !user_out) return JWT_BAD;
    if (sub_cap == 0 || user_cap == 0) return JWT_BAD;
    const char *tend = tok + tlen;
    const char *d1 = memchr(tok, '.', tlen);
    if (!d1) return JWT_BAD;
    const char *d2 = memchr(d1 + 1, '.', (size_t)(tend - (d1 + 1)));
    if (!d2) return JWT_BAD;
    if (memchr(d2 + 1, '.', (size_t)(tend - (d2 + 1)))) return JWT_BAD;
    size_t hlen = (size_t)(d1 - tok);
    size_t plen = (size_t)(d2 - (d1 + 1));
    const char *sp = d2 + 1;
    size_t slen = (size_t)(tend - sp);
    if (hlen == 0 || plen == 0 || slen == 0) return JWT_BAD;
    if (hlen > 1024 || plen > 6000 || slen > 128) return JWT_BAD;

    /* Header must decode and contain "alg":"HS256" (ws-tolerant). */
    unsigned char hbuf[768];
    size_t hlen2 = 0;
    if (b64url_dec(tok, hlen, hbuf, sizeof hbuf, &hlen2) != 0) return JWT_BAD;
    if (!json_header_is_hs256((const char *)hbuf, hlen2)) return JWT_BAD;

    /* Signature: base64url decode, must be 32 bytes. */
    unsigned char sig[64];
    size_t siglen = 0;
    if (b64url_dec(sp, slen, sig, sizeof sig, &siglen) != 0) return JWT_BAD;
    if (siglen != 32) return JWT_BAD;

    /* HMAC-SHA256(secret, "h.p") with constant-time compare. */
    unsigned char mac[32];
    unsigned int mlen = 0;
    size_t secret_len;
    if (secret == t_sec) {
        secret_len = t_seclen;
    } else {
        secret_len = strlen(secret);
        t_sec = secret;
        t_seclen = secret_len;
    }
    if (!HMAC(EVP_sha256(), secret, (int)secret_len,
              (const unsigned char *)tok, (size_t)(d2 - tok), mac, &mlen))
        return JWT_BAD;
    if (mlen != 32) return JWT_BAD;
    unsigned diff = 0;
    for (int i = 0; i < 32; i++) diff |= (unsigned)(mac[i] ^ sig[i]);
    if (__builtin_expect(diff != 0, 0)) return JWT_BAD;

    /* Payload claims via mini-scan (no full JSON lib). */
    static _Thread_local unsigned char pbuf[8192];
    size_t plen2 = 0;
    if (b64url_dec(d1 + 1, plen, pbuf, sizeof pbuf, &plen2) != 0) return JWT_BAD;

    /* sub must be a JSON string "[1-9][0-9]*" (not int, no leading zero). */
    char sub[32];
    if (json_scan_string((const char *)pbuf, plen2, "sub", sub, sizeof sub) != 0)
        return JWT_PAYLOAD;
    size_t sn = strlen(sub);
    if (sn == 0 || sn > 19 || sub[0] < '1' || sub[0] > '9') return JWT_PAYLOAD;
    long long uid = 0;
    for (size_t i = 0; i < sn; i++) {
        if (sub[i] < '0' || sub[i] > '9') return JWT_PAYLOAD;
        int dg = sub[i] - '0';
        if (uid > (LLONG_MAX - dg) / 10) return JWT_PAYLOAD;
        uid = uid * 10 + dg;
    }
    if (uid <= 0) return JWT_PAYLOAD;

    /* username must be a JSON string; used as-is for author. */
    if (json_scan_string((const char *)pbuf, plen2, "username", user_out, user_cap) != 0)
        return JWT_PAYLOAD;

    /* exp must be a JSON number greater than now; iat ignored.
     * Pass 9: 1s-tick-cached wall clock (1s granularity is fine for exp). */
    long long exp = 0;
    if (json_scan_number((const char *)pbuf, plen2, "exp", &exp) != 0)
        return JWT_BAD;
    if (exp <= (long long)net_now_wall()) return JWT_BAD;

    {
        size_t c = sn < sub_cap - 1 ? sn : sub_cap - 1;
        memcpy(sub_out, sub, c);
        sub_out[c] = '\0';
    }
    if (uid_out) *uid_out = uid;
    return JWT_OK;
}
