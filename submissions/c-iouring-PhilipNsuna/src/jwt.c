#include "jwt.h"
#include "http.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>

static int8_t b64v[256];
static void b64_init(void) {
    static int done;
    if (done) return;
    memset(b64v, -1, sizeof b64v);
    const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (int i = 0; i < 64; i++) b64v[(uint8_t)a[i]] = (int8_t)i;
    done = 1;
}

long b64url_decode(const uint8_t *s, size_t n, uint8_t *dst, size_t cap) {
    b64_init();
    if (n % 4 == 1) return -1;
    size_t out = n / 4 * 3 + (n % 4 ? n % 4 - 1 : 0);
    if (out > cap) return -1;
    uint32_t acc = 0; int bits = 0; size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        int8_t v = b64v[s[i]];
        if (v < 0) return -1;
        acc = acc << 6 | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; dst[o++] = (uint8_t)(acc >> bits); }
    }
    return (long)o;
}

void verifier_init(verifier_t *v, const char *secret, size_t len) { hmac_key_init(&v->key, secret, len); }

static int ct_equal(const uint8_t *a, const uint8_t *b, size_t n) { uint8_t d = 0; for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i]; return d == 0; }

int jwt_verify(verifier_t *v, const uint8_t *auth, size_t n, int64_t now, int64_t *uid, const uint8_t **name, size_t *name_len) {
    if (n < 7 || memcmp(auth, "Bearer ", 7)) return AUTH_MISSING;
    const uint8_t *tok = auth + 7; size_t tl = n - 7;
    const uint8_t *d1 = memchr(tok, '.', tl); if (!d1) return AUTH_INVALID;
    const uint8_t *d2 = memchr(d1 + 1, '.', tl - (size_t)(d1 + 1 - tok)); if (!d2) return AUTH_INVALID;
    if (memchr(d2 + 1, '.', tl - (size_t)(d2 + 1 - tok))) return AUTH_INVALID;
    // header
    uint8_t *buf = v->scratch;
    long hl = b64url_decode(tok, (size_t)(d1 - tok), buf, 512);
    if (hl < 0 || !json_valid(buf, (size_t)hl)) return AUTH_INVALID;
    const uint8_t *raw; size_t rl;
    if (json_object_get(buf, (size_t)hl, "alg", &raw, &rl) != JK_STRING || rl != 5 || memcmp(raw, "HS256", 5)) return AUTH_INVALID;
    // signature
    uint8_t sig[48];
    long sl = b64url_decode(d2 + 1, tl - (size_t)(d2 + 1 - tok), sig, sizeof sig);
    if (sl != 32) return AUTH_INVALID;
    uint8_t mac[32];
    hmac_sha256(&v->key, tok, (size_t)(d2 - tok), mac);
    if (!ct_equal(mac, sig, 32)) return AUTH_INVALID;
    // payload
    long pl = b64url_decode(d1 + 1, (size_t)(d2 - d1 - 1), buf, 1024);
    if (pl < 0 || !json_valid(buf, (size_t)pl)) return AUTH_INVALID;
    if (json_object_get(buf, (size_t)pl, "exp", &raw, &rl) != JK_NUMBER) return AUTH_INVALID;
    char tmp[32];
    if (rl >= sizeof tmp) return AUTH_INVALID;
    memcpy(tmp, raw, rl); tmp[rl] = 0;
    double e = strtod(tmp, NULL);
    if ((int64_t)e <= now) return AUTH_INVALID;
    if (json_object_get(buf, (size_t)pl, "sub", &raw, &rl) != JK_STRING || !parse_id(raw, rl, uid)) return AUTH_BAD_PAYLOAD;
    if (json_object_get(buf, (size_t)pl, "username", &raw, &rl) != JK_STRING) return AUTH_BAD_PAYLOAD;
    uint8_t *dst = buf + 1024;
    if (rl > 1000) return AUTH_BAD_PAYLOAD;
    long ul = json_unescape(raw, rl, dst);
    if (ul < 0) return AUTH_BAD_PAYLOAD;
    *name = dst; *name_len = (size_t)ul;
    return AUTH_OK;
}
