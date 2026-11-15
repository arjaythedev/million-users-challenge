// A strict JSON reader for request bodies and token claims (nothing is built: the caller names the members it
// wants and gets their places in the bytes), JSON string escaping, and JavaScript's String.trim.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <emmintrin.h>

typedef struct { const char *s; uint32_t n; uint8_t type; uint8_t esc; } jval_t;   // type: 0 absent, 's' 'n' 'o' 'a' 't' 'f' 'z'

static inline const char *j_ws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

// p is just past the opening quote. Returns the closing quote, or NULL.
static const char *j_string(const char *p, const char *e, uint8_t *esc) {
    for (; p < e; p++) {
        uint8_t c = (uint8_t)*p;
        if (c == '"') return p;
        if (c < 0x20) return NULL;
        if (c == '\\') {
            if (esc) *esc = 1;
            if (++p >= e) return NULL;
            switch (*p) {
            case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't': break;
            case 'u':
                if (e - p < 5) return NULL;
                for (int i = 1; i <= 4; i++) {
                    uint8_t h = (uint8_t)p[i];
                    if (!((h >= '0' && h <= '9') || ((h | 32) >= 'a' && (h | 32) <= 'f'))) return NULL;
                }
                p += 4;
                break;
            default: return NULL;
            }
        }
    }
    return NULL;
}

static const char *j_number(const char *p, const char *e) {
    if (p < e && *p == '-') p++;
    if (p >= e) return NULL;
    if (*p == '0') p++;
    else if (*p >= '1' && *p <= '9') { while (p < e && *p >= '0' && *p <= '9') p++; }
    else return NULL;
    if (p < e && *p == '.') {
        p++;
        if (p >= e || *p < '0' || *p > '9') return NULL;
        while (p < e && *p >= '0' && *p <= '9') p++;
    }
    if (p < e && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < e && (*p == '+' || *p == '-')) p++;
        if (p >= e || *p < '0' || *p > '9') return NULL;
        while (p < e && *p >= '0' && *p <= '9') p++;
    }
    return p;
}

// Returns the first byte after the value at p, or NULL if it isn't valid JSON. Fills v (if given) with its place.
static const char *j_value(const char *p, const char *e, int depth, jval_t *v) {
    if (p >= e || depth > 64) return NULL;
    const char *q;
    uint8_t esc = 0;
    switch (*p) {
    case '"':
        q = j_string(p + 1, e, &esc);
        if (!q) return NULL;
        if (v) { v->s = p + 1; v->n = (uint32_t)(q - p - 1); v->type = 's'; v->esc = esc; }
        return q + 1;
    case '{':
        q = j_ws(p + 1, e);
        if (q < e && *q == '}') q++;
        else for (;;) {
            if (q >= e || *q != '"') return NULL;
            q = j_string(q + 1, e, NULL);
            if (!q) return NULL;
            q = j_ws(q + 1, e);
            if (q >= e || *q != ':') return NULL;
            q = j_value(j_ws(q + 1, e), e, depth + 1, NULL);
            if (!q) return NULL;
            q = j_ws(q, e);
            if (q < e && *q == ',') { q = j_ws(q + 1, e); continue; }
            if (q < e && *q == '}') { q++; break; }
            return NULL;
        }
        if (v) { v->s = p; v->n = (uint32_t)(q - p); v->type = 'o'; }
        return q;
    case '[':
        q = j_ws(p + 1, e);
        if (q < e && *q == ']') q++;
        else for (;;) {
            q = j_value(q, e, depth + 1, NULL);
            if (!q) return NULL;
            q = j_ws(q, e);
            if (q < e && *q == ',') { q = j_ws(q + 1, e); continue; }
            if (q < e && *q == ']') { q++; break; }
            return NULL;
        }
        if (v) { v->s = p; v->n = (uint32_t)(q - p); v->type = 'a'; }
        return q;
    case 't':
        if (e - p < 4 || memcmp(p, "true", 4)) return NULL;
        if (v) { v->s = p; v->n = 4; v->type = 't'; }
        return p + 4;
    case 'f':
        if (e - p < 5 || memcmp(p, "false", 5)) return NULL;
        if (v) { v->s = p; v->n = 5; v->type = 'f'; }
        return p + 5;
    case 'n':
        if (e - p < 4 || memcmp(p, "null", 4)) return NULL;
        if (v) { v->s = p; v->n = 4; v->type = 'z'; }
        return p + 4;
    default:
        q = j_number(p, e);
        if (!q) return NULL;
        if (v) { v->s = p; v->n = (uint32_t)(q - p); v->type = 'n'; }
        return q;
    }
}

static inline int j_hex(uint8_t h) { return h <= '9' ? h - '0' : (h | 32) - 'a' + 10; }

// Unescapes validated string content into out (never longer than the input). Returns the length.
static uint32_t j_unescape(const char *s, uint32_t n, char *out) {
    char *o = out;
    const char *e = s + n;
    while (s < e) {
        if (*s != '\\') { *o++ = *s++; continue; }
        s++;
        switch (*s++) {
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'u': {
            uint32_t cp = (uint32_t)(j_hex((uint8_t)s[0]) << 12 | j_hex((uint8_t)s[1]) << 8 | j_hex((uint8_t)s[2]) << 4 | j_hex((uint8_t)s[3]));
            s += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && e - s >= 6 && s[0] == '\\' && s[1] == 'u') {
                uint32_t lo = (uint32_t)(j_hex((uint8_t)s[2]) << 12 | j_hex((uint8_t)s[3]) << 8 | j_hex((uint8_t)s[4]) << 4 | j_hex((uint8_t)s[5]));
                if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); s += 6; }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;      // a lone surrogate has no UTF-8 form
            if (cp < 0x80) *o++ = (char)cp;
            else if (cp < 0x800) { *o++ = (char)(0xC0 | cp >> 6); *o++ = (char)(0x80 | (cp & 63)); }
            else if (cp < 0x10000) { *o++ = (char)(0xE0 | cp >> 12); *o++ = (char)(0x80 | ((cp >> 6) & 63)); *o++ = (char)(0x80 | (cp & 63)); }
            else { *o++ = (char)(0xF0 | cp >> 18); *o++ = (char)(0x80 | ((cp >> 12) & 63)); *o++ = (char)(0x80 | ((cp >> 6) & 63)); *o++ = (char)(0x80 | (cp & 63)); }
            break;
        }
        default: *o++ = s[-1]; break;      // " \ /
        }
    }
    return (uint32_t)(o - out);
}

// Checks that p[0..n) is one JSON document. If it is an object, vals[i] gets the place of member keys[i]
// (the last one, as JSON.parse keeps), type 0 if absent. Returns 0 if malformed, 1 if valid and an object,
// 2 if valid and something else.
static int j_doc(const char *p, size_t n, const char *const *keys, int nkeys, jval_t *vals) {
    const char *e = p + n;
    for (int i = 0; i < nkeys; i++) vals[i].type = 0;
    p = j_ws(p, e);
    if (p >= e) return 0;
    if (*p != '{') {
        const char *q = j_value(p, e, 0, NULL);
        if (!q || j_ws(q, e) != e) return 0;
        return 2;
    }
    const char *q = j_ws(p + 1, e);
    if (q < e && *q == '}') q++;
    else for (;;) {
        if (q >= e || *q != '"') return 0;
        uint8_t kesc = 0;
        const char *ks = q + 1, *ke = j_string(ks, e, &kesc);
        if (!ke) return 0;
        char kb[64];
        uint32_t kl = (uint32_t)(ke - ks);
        const char *kp = ks;
        if (kesc) { if (kl < sizeof kb) { kl = j_unescape(ks, kl, kb); kp = kb; } else kl = 0xffffffff; }
        q = j_ws(ke + 1, e);
        if (q >= e || *q != ':') return 0;
        q = j_ws(q + 1, e);
        jval_t v = {0};
        const char *ve = j_value(q, e, 1, &v);
        if (!ve) return 0;
        for (int i = 0; i < nkeys; i++)
            if (strlen(keys[i]) == kl && !memcmp(keys[i], kp, kl)) vals[i] = v;
        q = j_ws(ve, e);
        if (q < e && *q == ',') { q = j_ws(q + 1, e); continue; }
        if (q < e && *q == '}') { q++; break; }
        return 0;
    }
    return j_ws(q, e) == e ? 1 : 0;
}

// ---- output escaping ----
// Copies s[0..n) into o as JSON string content. Returns the end. Needs up to 6 * n bytes.
static char *j_escape(char *o, const uint8_t *s, uint32_t n) {
    static const char HEX[] = "0123456789abcdef";
    uint32_t i = 0;
    const __m128i q = _mm_set1_epi8('"'), bs = _mm_set1_epi8('\\'), lim = _mm_set1_epi8(0x1f);
    while (i < n) {
        while (i + 16 <= n) {
            __m128i v = _mm_loadu_si128((const __m128i *)(s + i));
            __m128i bad = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, q), _mm_cmpeq_epi8(v, bs)),
                                       _mm_cmpeq_epi8(_mm_min_epu8(v, lim), v));
            _mm_storeu_si128((__m128i *)o, v);
            int m = _mm_movemask_epi8(bad);
            if (m) { int k = __builtin_ctz((unsigned)m); o += k; i += (uint32_t)k; goto one; }
            o += 16; i += 16;
        }
        if (i >= n) break;
    one:;
        uint8_t c = s[i++];
        if (c == '"' || c == '\\') { *o++ = '\\'; *o++ = (char)c; }
        else if (c >= 0x20) *o++ = (char)c;
        else switch (c) {
            case '\b': *o++ = '\\'; *o++ = 'b'; break;
            case '\f': *o++ = '\\'; *o++ = 'f'; break;
            case '\n': *o++ = '\\'; *o++ = 'n'; break;
            case '\r': *o++ = '\\'; *o++ = 'r'; break;
            case '\t': *o++ = '\\'; *o++ = 't'; break;
            default: *o++ = '\\'; *o++ = 'u'; *o++ = '0'; *o++ = '0'; *o++ = HEX[c >> 4]; *o++ = HEX[c & 15];
        }
    }
    return o;
}

// ---- String.prototype.trim on UTF-8 ----
// length of the white space sequence at s (0 if none)
static inline uint32_t js_ws_at(const uint8_t *s, uint32_t n) {
    uint8_t c = s[0];
    if (c == ' ' || (c >= 9 && c <= 13)) return 1;
    if (c == 0xC2 && n >= 2 && s[1] == 0xA0) return 2;
    if (n >= 3) {
        if (c == 0xE1 && s[1] == 0x9A && s[2] == 0x80) return 3;
        if (c == 0xE2 && s[1] == 0x80 && ((s[2] >= 0x80 && s[2] <= 0x8A) || s[2] == 0xA8 || s[2] == 0xA9 || s[2] == 0xAF)) return 3;
        if (c == 0xE2 && s[1] == 0x81 && s[2] == 0x9F) return 3;
        if (c == 0xE3 && s[1] == 0x80 && s[2] == 0x80) return 3;
        if (c == 0xEF && s[1] == 0xBB && s[2] == 0xBF) return 3;
    }
    return 0;
}

static void js_trim(const uint8_t **ps, uint32_t *pn) {
    const uint8_t *s = *ps;
    uint32_t n = *pn, k;
    while (n && (k = js_ws_at(s, n))) { s += k; n -= k; }
    for (;;) {
        if (!n) break;
        if (js_ws_at(s + n - 1, 1)) { n--; continue; }
        if (n >= 2 && js_ws_at(s + n - 2, 2) == 2) { n -= 2; continue; }
        if (n >= 3 && js_ws_at(s + n - 3, 3) == 3) { n -= 3; continue; }
        break;
    }
    *ps = s;
    *pn = n;
}

static inline uint32_t utf8_codepoints(const uint8_t *s, uint32_t n) {
    uint32_t c = 0;
    for (uint32_t i = 0; i < n; i++) c += (s[i] & 0xC0) != 0x80;
    return c;
}
