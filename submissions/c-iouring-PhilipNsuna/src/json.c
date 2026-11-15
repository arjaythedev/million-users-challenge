#include "json.h"
#include <string.h>

#define MAX_DEPTH 64

static size_t ws(const uint8_t *b, size_t n, size_t i) {
    while (i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\n' || b[i] == '\r')) i++;
    return i;
}

static int ishex(uint8_t c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int hex4(const uint8_t *p, uint32_t *out) {
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t c = p[i];
        if (!ishex(c)) return 0;
        r = r << 4 | (uint32_t)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    *out = r;
    return 1;
}

// utf8_len: length of a valid UTF-8 sequence at p (within n), 0 if invalid.
static int utf8_len(const uint8_t *p, size_t n) {
    uint8_t c = p[0];
    if (c < 0x80) return 1;
    int len; uint32_t min;
    if ((c & 0xE0) == 0xC0) { len = 2; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { len = 3; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { len = 4; min = 0x10000; }
    else return 0;
    if ((size_t)len > n) return 0;
    uint32_t cp = c & (0xFF >> (len + 1));
    for (int i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        cp = cp << 6 | (p[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) return 0;
    return len;
}

static long skip_string(const uint8_t *b, size_t n, size_t i) { // b[i] == '"'; returns index after the closing quote
    i++;
    while (i < n) {
        uint8_t c = b[i];
        if (c == '"') return (long)i + 1;
        if (c == '\\') {
            if (++i >= n) return -1;
            switch (b[i]) {
            case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't': i++; break;
            case 'u': { uint32_t x; if (i + 4 >= n || !hex4(b + i + 1, &x)) return -1; i += 5; break; }
            default: return -1;
            }
        } else if (c < 0x20) return -1;
        else if (c < 0x80) i++;
        else { int l = utf8_len(b + i, n - i); if (!l) return -1; i += l; }
    }
    return -1;
}

static long skip_number(const uint8_t *b, size_t n, size_t i) {
    size_t s = i;
    if (i < n && b[i] == '-') i++;
    if (i >= n) return -1;
    if (b[i] == '0') i++;
    else if (b[i] >= '1' && b[i] <= '9') while (i < n && b[i] >= '0' && b[i] <= '9') i++;
    else return -1;
    if (i < n && b[i] == '.') { i++; if (i >= n || b[i] < '0' || b[i] > '9') return -1; while (i < n && b[i] >= '0' && b[i] <= '9') i++; }
    if (i < n && (b[i] == 'e' || b[i] == 'E')) {
        i++;
        if (i < n && (b[i] == '+' || b[i] == '-')) i++;
        if (i >= n || b[i] < '0' || b[i] > '9') return -1;
        while (i < n && b[i] >= '0' && b[i] <= '9') i++;
    }
    return i > s ? (long)i : -1;
}

static long skip_value(const uint8_t *b, size_t n, size_t i, int depth) {
    if (i >= n || depth > MAX_DEPTH) return -1;
    switch (b[i]) {
    case '{': {
        i = ws(b, n, i + 1);
        if (i < n && b[i] == '}') return (long)i + 1;
        for (;;) {
            if (i >= n || b[i] != '"') return -1;
            long e = skip_string(b, n, i); if (e < 0) return -1;
            i = ws(b, n, (size_t)e);
            if (i >= n || b[i] != ':') return -1;
            i = ws(b, n, i + 1);
            e = skip_value(b, n, i, depth + 1); if (e < 0) return -1;
            i = ws(b, n, (size_t)e);
            if (i >= n) return -1;
            if (b[i] == '}') return (long)i + 1;
            if (b[i] != ',') return -1;
            i = ws(b, n, i + 1);
        }
    }
    case '[': {
        i = ws(b, n, i + 1);
        if (i < n && b[i] == ']') return (long)i + 1;
        for (;;) {
            long e = skip_value(b, n, i, depth + 1); if (e < 0) return -1;
            i = ws(b, n, (size_t)e);
            if (i >= n) return -1;
            if (b[i] == ']') return (long)i + 1;
            if (b[i] != ',') return -1;
            i = ws(b, n, i + 1);
        }
    }
    case '"': return skip_string(b, n, i);
    case 't': return n - i >= 4 && !memcmp(b + i, "true", 4) ? (long)i + 4 : -1;
    case 'f': return n - i >= 5 && !memcmp(b + i, "false", 5) ? (long)i + 5 : -1;
    case 'n': return n - i >= 4 && !memcmp(b + i, "null", 4) ? (long)i + 4 : -1;
    default: return skip_number(b, n, i);
    }
}

bool json_valid(const uint8_t *b, size_t n) {
    size_t i = ws(b, n, 0);
    long e = skip_value(b, n, i, 0);
    if (e < 0) return false;
    return ws(b, n, (size_t)e) == n;
}

static int kind_of(uint8_t c) {
    switch (c) {
    case '"': return JK_STRING; case '{': return JK_OBJECT; case '[': return JK_ARRAY;
    case 't': return JK_TRUE; case 'f': return JK_FALSE; case 'n': return JK_NULL; default: return JK_NUMBER;
    }
}

int json_object_get(const uint8_t *b, size_t n, const char *key, const uint8_t **raw, size_t *rawlen) {
    size_t klen = strlen(key);
    int kind = JK_NONE;
    size_t i = ws(b, n, 0);
    if (i >= n || b[i] != '{') return JK_NONE;
    i = ws(b, n, i + 1);
    while (i < n && b[i] == '"') {
        size_t ks = i + 1;
        long ke = skip_string(b, n, i);
        size_t kl = (size_t)ke - 1 - ks;
        i = ws(b, n, (size_t)ke);
        i = ws(b, n, i + 1);
        size_t vs = i;
        long ve = skip_value(b, n, i, 0);
        int match = kl == klen && !memcmp(b + ks, key, klen);
        if (!match && memchr(b + ks, '\\', kl)) { // key written with escapes
            uint8_t tmp[64];
            long ul = kl <= 64 ? json_unescape(b + ks, kl, tmp) : -1;
            match = ul == (long)klen && !memcmp(tmp, key, klen);
        }
        if (match) {
            kind = kind_of(b[vs]);
            if (kind == JK_STRING) { *raw = b + vs + 1; *rawlen = (size_t)ve - vs - 2; }
            else { *raw = b + vs; *rawlen = (size_t)ve - vs; }
        }
        i = ws(b, n, (size_t)ve);
        if (i < n && b[i] == ',') i = ws(b, n, i + 1);
    }
    return kind;
}

static size_t put_utf8(uint8_t *d, uint32_t cp) {
    if (cp < 0x80) { d[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800) { d[0] = 0xC0 | cp >> 6; d[1] = 0x80 | (cp & 0x3F); return 2; }
    if (cp < 0x10000) { d[0] = 0xE0 | cp >> 12; d[1] = 0x80 | (cp >> 6 & 0x3F); d[2] = 0x80 | (cp & 0x3F); return 3; }
    d[0] = 0xF0 | cp >> 18; d[1] = 0x80 | (cp >> 12 & 0x3F); d[2] = 0x80 | (cp >> 6 & 0x3F); d[3] = 0x80 | (cp & 0x3F); return 4;
}

long json_unescape(const uint8_t *raw, size_t n, uint8_t *dst) {
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        uint8_t c = raw[i];
        if (c != '\\') { dst[o++] = c; i++; continue; }
        if (++i >= n) return -1;
        switch (raw[i]) {
        case '"': dst[o++] = '"'; i++; break;
        case '\\': dst[o++] = '\\'; i++; break;
        case '/': dst[o++] = '/'; i++; break;
        case 'b': dst[o++] = '\b'; i++; break;
        case 'f': dst[o++] = '\f'; i++; break;
        case 'n': dst[o++] = '\n'; i++; break;
        case 'r': dst[o++] = '\r'; i++; break;
        case 't': dst[o++] = '\t'; i++; break;
        case 'u': {
            uint32_t r;
            if (i + 4 >= n || !hex4(raw + i + 1, &r)) return -1;
            i += 5;
            if (r >= 0xD800 && r < 0xDC00) {
                uint32_t r2;
                if (i + 5 < n && raw[i] == '\\' && raw[i + 1] == 'u' && hex4(raw + i + 2, &r2) && r2 >= 0xDC00 && r2 < 0xE000) {
                    r = 0x10000 + ((r - 0xD800) << 10) + (r2 - 0xDC00);
                    i += 6;
                } else r = 0xFFFD;
            } else if (r >= 0xDC00 && r < 0xE000) r = 0xFFFD;
            o += put_utf8(dst + o, r);
            break;
        }
        default: return -1;
        }
    }
    return (long)o;
}

size_t json_append_string(uint8_t *dst, size_t pos, const uint8_t *s, size_t n) {
    static const char hexd[] = "0123456789abcdef";
    dst[pos++] = '"';
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (c >= 0x20 && c != '"' && c != '\\') continue;
        memcpy(dst + pos, s + start, i - start); pos += i - start;
        dst[pos++] = '\\';
        switch (c) {
        case '"': dst[pos++] = '"'; break;
        case '\\': dst[pos++] = '\\'; break;
        case '\n': dst[pos++] = 'n'; break;
        case '\r': dst[pos++] = 'r'; break;
        case '\t': dst[pos++] = 't'; break;
        case '\b': dst[pos++] = 'b'; break;
        case '\f': dst[pos++] = 'f'; break;
        default: dst[pos++] = 'u'; dst[pos++] = '0'; dst[pos++] = '0'; dst[pos++] = (uint8_t)hexd[c >> 4]; dst[pos++] = (uint8_t)hexd[c & 15];
        }
        start = i + 1;
    }
    memcpy(dst + pos, s + start, n - start); pos += n - start;
    dst[pos++] = '"';
    return pos;
}

static int js_ws(uint32_t r) {
    switch (r) {
    case ' ': case '\t': case '\n': case '\v': case '\f': case '\r': case 0xA0: case 0x1680:
    case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000: case 0xFEFF: return 1;
    }
    return r >= 0x2000 && r <= 0x200A;
}

static int decode(const uint8_t *p, size_t n, uint32_t *cp) { // valid UTF-8 assumed (the JSON scanner checked)
    int l = utf8_len(p, n);
    if (l <= 0) { *cp = p[0]; return 1; }
    uint32_t c = p[0] & (0xFF >> (l == 1 ? 0 : l + 1));
    for (int i = 1; i < l; i++) c = c << 6 | (p[i] & 0x3F);
    *cp = c;
    return l;
}

const uint8_t *trim_js(const uint8_t *s, size_t n, size_t *len) {
    while (n) { uint32_t cp; int l = decode(s, n, &cp); if (!js_ws(cp)) break; s += l; n -= (size_t)l; }
    while (n) { // last rune
        size_t i = n - 1;
        while (i > 0 && (s[i] & 0xC0) == 0x80) i--;
        uint32_t cp; decode(s + i, n - i, &cp);
        if (!js_ws(cp)) break;
        n = i;
    }
    *len = n;
    return s;
}

size_t utf8_count(const uint8_t *s, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++) if ((s[i] & 0xC0) != 0x80) c++;
    return c;
}
