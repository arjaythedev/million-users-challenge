#include "json.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

const char *JERR_NOT_FOUND = "{\"error\":\"not found\"}";
const char *JERR_INVALID_POST_ID = "{\"error\":\"invalid post id\"}";
const char *JERR_POST_NOT_FOUND = "{\"error\":\"post not found\"}";
const char *JERR_MISSING_BEARER = "{\"error\":\"missing bearer token\"}";
const char *JERR_INVALID_TOKEN = "{\"error\":\"invalid or expired token\"}";
const char *JERR_INVALID_PAYLOAD = "{\"error\":\"invalid token payload\"}";
const char *JERR_MALFORMED_JSON = "{\"error\":\"malformed JSON body\"}";
const char *JERR_BODY_REQUIRED = "{\"error\":\"body is required\"}";
const char *JERR_BODY_TOO_LONG = "{\"error\":\"body must be at most 500 characters\"}";
const char *JERR_INTERNAL = "{\"error\":\"internal server error\"}";

static const char E_00[] = "\\u0000";
static const char E_01[] = "\\u0001";
static const char E_02[] = "\\u0002";
static const char E_03[] = "\\u0003";
static const char E_04[] = "\\u0004";
static const char E_05[] = "\\u0005";
static const char E_06[] = "\\u0006";
static const char E_07[] = "\\u0007";
static const char E_08[] = "\\b";
static const char E_09[] = "\\t";
static const char E_0A[] = "\\n";
static const char E_0B[] = "\\u000b";
static const char E_0C[] = "\\f";
static const char E_0D[] = "\\r";
static const char E_0E[] = "\\u000e";
static const char E_0F[] = "\\u000f";
static const char E_10[] = "\\u0010";
static const char E_11[] = "\\u0011";
static const char E_12[] = "\\u0012";
static const char E_13[] = "\\u0013";
static const char E_14[] = "\\u0014";
static const char E_15[] = "\\u0015";
static const char E_16[] = "\\u0016";
static const char E_17[] = "\\u0017";
static const char E_18[] = "\\u0018";
static const char E_19[] = "\\u0019";
static const char E_1A[] = "\\u001a";
static const char E_1B[] = "\\u001b";
static const char E_1C[] = "\\u001c";
static const char E_1D[] = "\\u001d";
static const char E_1E[] = "\\u001e";
static const char E_1F[] = "\\u001f";
static const char E_22[] = "\\\"";
static const char E_5C[] = "\\\\";
static const char *const ESC[256] = {
    E_00, E_01, E_02, E_03, E_04, E_05, E_06, E_07,
    E_08, E_09, E_0A, E_0B, E_0C, E_0D, E_0E, E_0F,
    E_10, E_11, E_12, E_13, E_14, E_15, E_16, E_17,
    E_18, E_19, E_1A, E_1B, E_1C, E_1D, E_1E, E_1F,
    0, 0, E_22, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, E_5C, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};
static const unsigned char ESCLEN[256] = {
    6, 6, 6, 6, 6, 6, 6, 6,
    2, 2, 2, 6, 2, 2, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6,
    0, 0, 2, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 2, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

/* Table-driven escape: identical output to the branchy version (same caps,
 * same lowercase hex for control chars, same truncation behavior).
 * Pass 9: bulk-copy runs of plain bytes (memcpy) instead of byte-at-a-time
 * when no escape is needed; per-char output for runs is bit-identical. */
size_t json_escape_n(const char *src, size_t n, char *dst, size_t cap) {
    size_t o = 0;
    if (cap == 0) return 0;
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)src[i];
        if (ESC[c] == 0) {
            size_t j = i + 1;
            while (j < n && ESC[(unsigned char)src[j]] == 0) j++;
            size_t run = j - i;
            size_t room = (o + 1 >= cap) ? 0 : (cap - 1 - o);
            size_t k = run < room ? run : room;
            if (k) { memcpy(dst + o, src + i, k); o += k; i += k; }
            if (k < run) break;
        } else {
            size_t el = ESCLEN[c];
            if (o + el >= cap) break;
            memcpy(dst + o, ESC[c], el);
            o += el;
            i++;
        }
    }
    if (o < cap) dst[o] = '\0';
    else dst[cap - 1] = '\0';
    return o;
}

size_t json_escape(const char *src, char *dst, size_t cap) {
    return json_escape_n(src, strlen(src), dst, cap);
}

size_t json_render_health(char *out, size_t cap, long uptime_s) {
    int n = snprintf(out, cap, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":%ld}", uptime_s);
    if (n < 0) { if (cap) out[0] = '\0'; return 0; }
    if ((size_t)n >= cap) return cap ? cap - 1 : 0;
    return (size_t)n;
}

size_t json_render_degraded(char *out, size_t cap, const char *errmsg) {
    const char *pre = "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"";
    const char *suf = "\"}";
    size_t o = 0;
    size_t pl = strlen(pre), sl = strlen(suf);
    char esc[3072];
    if (cap == 0) return 0;
    if (!errmsg || !*errmsg) errmsg = "db unreachable";
    json_escape_n(errmsg, strlen(errmsg), esc, sizeof esc);
    size_t el = strlen(esc);
    if (pl + el + sl + 1 > cap) { out[0] = '\0'; return 0; }
    memcpy(out, pre, pl); o = pl;
    memcpy(out + o, esc, el); o += el;
    memcpy(out + o, suf, sl); o += sl;
    out[o] = '\0';
    return o;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int emit_utf8(char *out, size_t cap, size_t *o, unsigned cp) {
    if (cp < 0x80) {
        if (*o + 1 >= cap) return -1;
        out[(*o)++] = (char)cp;
    } else if (cp < 0x800) {
        if (*o + 2 >= cap) return -1;
        out[(*o)++] = (char)(0xC0 | (cp >> 6));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        if (*o + 3 >= cap) return -1;
        out[(*o)++] = (char)(0xE0 | (cp >> 12));
        out[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        if (*o + 4 >= cap) return -1;
        out[(*o)++] = (char)(0xF0 | (cp >> 18));
        out[(*o)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        return -1;
    }
    return 0;
}

size_t json_decode_string(const char *s, size_t n, char *out, size_t cap) {
    size_t o = 0;
    if (cap == 0) return (size_t)-1;
    for (size_t i = 0; i < n;) {
        char c = s[i];
        if (c != '\\') {
            if (o + 1 >= cap) return (size_t)-1;
            out[o++] = c;
            i++;
            continue;
        }
        i++;
        if (i >= n) return (size_t)-1;
        char e = s[i++];
        switch (e) {
        case '"': if (o + 1 >= cap) return (size_t)-1; out[o++] = '"'; break;
        case '\\': if (o + 1 >= cap) return (size_t)-1; out[o++] = '\\'; break;
        case '/': if (o + 1 >= cap) return (size_t)-1; out[o++] = '/'; break;
        case 'b': if (o + 1 >= cap) return (size_t)-1; out[o++] = '\b'; break;
        case 'f': if (o + 1 >= cap) return (size_t)-1; out[o++] = '\f'; break;
        case 'n': if (o + 1 >= cap) return (size_t)-1; out[o++] = '\n'; break;
        case 'r': if (o + 1 >= cap) return (size_t)-1; out[o++] = '\r'; break;
        case 't': if (o + 1 >= cap) return (size_t)-1; out[o++] = '\t'; break;
        case 'u': {
            unsigned cp = 0;
            if (i + 4 > n) return (size_t)-1;
            for (int k = 0; k < 4; k++) {
                int v = hexv(s[i + k]);
                if (v < 0) return (size_t)-1;
                cp = (cp << 4) | (unsigned)v;
            }
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                /* high surrogate: must be followed by \uDC00..DFFF */
                unsigned lo = 0;
                if (i + 6 > n || s[i] != '\\' || s[i + 1] != 'u') return (size_t)-1;
                for (int k = 0; k < 4; k++) {
                    int v = hexv(s[i + 2 + k]);
                    if (v < 0) return (size_t)-1;
                    lo = (lo << 4) | (unsigned)v;
                }
                if (lo < 0xDC00 || lo > 0xDFFF) return (size_t)-1;
                i += 6;
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return (size_t)-1; /* lone low surrogate */
            }
            if (emit_utf8(out, cap, &o, cp) != 0) return (size_t)-1;
            break;
        }
        default:
            return (size_t)-1;
        }
    }
    if (o >= cap) return (size_t)-1;
    out[o] = '\0';
    return o;
}

static const char *skip_ws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* Validate a JSON string inner payload (escapes legal, no raw controls).
 * Also rejects lone surrogates / bad \uXXXX so skipped strings are held to
 * the same grammar as decoded ones (Greptile #2: invalid JSON must not be
 * silently accepted). Returns 1 valid, 0 invalid. */
static int jstr_inner_valid(const char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') {
            if (++i >= n) return 0;
            char e2 = s[i++];
            switch (e2) {
            case '"': case '\\': case '/': case 'b': case 'f':
            case 'n': case 'r': case 't':
                break;
            case 'u': {
                unsigned cp = 0;
                if (i + 4 > n) return 0;
                for (int k = 0; k < 4; k++) {
                    char h = s[i + k];
                    int v = (h >= '0' && h <= '9') ? h - '0'
                        : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                        : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                    if (v < 0) return 0;
                    cp = (cp << 4) | (unsigned)v;
                }
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned lo = 0;
                    if (i + 6 > n || s[i] != '\\' || s[i + 1] != 'u') return 0;
                    for (int k = 0; k < 4; k++) {
                        char h = s[i + 2 + k];
                        int v = (h >= '0' && h <= '9') ? h - '0'
                            : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                            : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                        if (v < 0) return 0;
                        lo = (lo << 4) | (unsigned)v;
                    }
                    if (lo < 0xDC00 || lo > 0xDFFF) return 0;
                    i += 6;
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return 0;
                }
                break;
            }
            default:
                return 0;
            }
            continue;
        }
        if (c < 0x20) return 0; /* raw control (incl NUL): malformed */
        i++;
    }
    return 1;
}

/* Skip one JSON value with full grammar validation (strings validated,
 * numbers strict, depth-capped containers). Returns past the value or NULL.
 * Depth cap 32: JWT payloads are flat; anything deeper fails closed. */
#define JSKIP_MAX_DEPTH 32
static const char *jval_end_d(const char *p, const char *e, int depth) {
    if (depth > JSKIP_MAX_DEPTH) return NULL;
    p = skip_ws(p, e);
    if (p >= e) return NULL;
    if (*p == '"') {
        const char *q = p + 1;
        int esc = 0, closed = 0;
        while (q < e) {
            char ch = *q;
            if (esc) esc = 0;
            else if (ch == '\\') esc = 1;
            else if (ch == '"') { closed = 1; break; }
            else if ((unsigned char)ch < 0x20) return NULL;
            q++;
        }
        if (!closed) return NULL;
        if (!jstr_inner_valid(p + 1, (size_t)(q - (p + 1)))) return NULL;
        return q + 1;
    }
    if (*p == '{') {
        p = skip_ws(p + 1, e);
        if (p < e && *p == '}') return p + 1;
        while (1) {
            if (p >= e || *p != '"') return NULL;
            const char *q = p + 1;
            int esc = 0, closed = 0;
            while (q < e) {
                char ch = *q;
                if (esc) esc = 0;
                else if (ch == '\\') esc = 1;
                else if (ch == '"') { closed = 1; break; }
                else if ((unsigned char)ch < 0x20) return NULL;
                q++;
            }
            if (!closed) return NULL;
            if (!jstr_inner_valid(p + 1, (size_t)(q - (p + 1)))) return NULL;
            p = skip_ws(q + 1, e);
            if (p >= e || *p != ':') return NULL;
            p = jval_end_d(p + 1, e, depth + 1);
            if (!p) return NULL;
            p = skip_ws(p, e);
            if (p >= e) return NULL;
            if (*p == ',') { p = skip_ws(p + 1, e); continue; }
            if (*p == '}') return p + 1;
            return NULL;
        }
    }
    if (*p == '[') {
        p = skip_ws(p + 1, e);
        if (p < e && *p == ']') return p + 1;
        while (1) {
            p = jval_end_d(p, e, depth + 1);
            if (!p) return NULL;
            p = skip_ws(p, e);
            if (p >= e) return NULL;
            if (*p == ',') { p = skip_ws(p + 1, e); continue; }
            if (*p == ']') return p + 1;
            return NULL;
        }
    }
    if (*p == 't') {
        if ((size_t)(e - p) >= 4 && memcmp(p, "true", 4) == 0) return p + 4;
        return NULL;
    }
    if (*p == 'f') {
        if ((size_t)(e - p) >= 5 && memcmp(p, "false", 5) == 0) return p + 5;
        return NULL;
    }
    if (*p == 'n') {
        if ((size_t)(e - p) >= 4 && memcmp(p, "null", 4) == 0) return p + 4;
        return NULL;
    }
    /* strict number: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)? */
    {
        const char *q = p;
        if (q < e && *q == '-') q++;
        if (q >= e) return NULL;
        if (*q == '0') { q++; }
        else if (*q >= '1' && *q <= '9') { while (q < e && *q >= '0' && *q <= '9') q++; }
        else return NULL;
        if (q < e && *q == '.') {
            q++;
            const char *fs = q;
            while (q < e && *q >= '0' && *q <= '9') q++;
            if (q == fs) return NULL;
        }
        if (q < e && (*q == 'e' || *q == 'E')) {
            q++;
            if (q < e && (*q == '+' || *q == '-')) q++;
            const char *es = q;
            while (q < e && *q >= '0' && *q <= '9') q++;
            if (q == es) return NULL;
        }
        if (q == p) return NULL;
        return q;
    }
}

/* Find top-level "key":value in a JSON object; returns the pointer to the
 * value start (past ':') or NULL (missing OR malformed -> fail closed).
 * Greptile #4: the old scan matched nested keys ({"extra":{"sub":"2"}} took
 * the inner "sub"); only depth-1 members are considered, keys are
 * escape-decoded before compare ({"\u0073ub":...} == "sub"), and the payload
 * must be a well-formed object (validated values, strict separators). */
static const char *find_key(const char *js, size_t len, const char *key) {
    const char *e = js + len;
    size_t kl = strlen(key);
    const char *p = skip_ws(js, e);
    if (p >= e || *p != '{') return NULL;
    p = skip_ws(p + 1, e);
    if (p >= e) return NULL;
    if (*p == '}') return NULL; /* empty object: key absent */
    const char *found = NULL;
    while (p < e) {
        if (*p != '"') return NULL;
        const char *q = p + 1;
        int esc = 0, closed = 0;
        while (q < e) {
            char ch = *q;
            if (esc) esc = 0;
            else if (ch == '\\') esc = 1;
            else if (ch == '"') { closed = 1; break; }
            else if ((unsigned char)ch < 0x20) return NULL;
            q++;
        }
        if (!closed) return NULL;
        size_t ilen = (size_t)(q - (p + 1));
        if (!jstr_inner_valid(p + 1, ilen)) return NULL;
        int match = 0;
        if (ilen == kl && memcmp(p + 1, key, kl) == 0) {
            match = 1; /* fast path: no escapes */
        } else if (memchr(p + 1, '\\', ilen) != NULL && ilen < 64) {
            char kd[64];
            if (json_decode_string(p + 1, ilen, kd, sizeof kd) != (size_t)-1 &&
                strlen(kd) == kl && memcmp(kd, key, kl) == 0)
                match = 1;
        }
        p = skip_ws(q + 1, e);
        if (p >= e || *p != ':') return NULL;
        p = skip_ws(p + 1, e);
        if (p >= e) return NULL;
        if (match && !found) found = p;
        p = jval_end_d(p, e, 0);
        if (!p) return NULL;
        p = skip_ws(p, e);
        if (p >= e) return NULL;
        if (*p == ',') { p = skip_ws(p + 1, e); if (p >= e) return NULL; continue; }
        if (*p == '}') {
            /* Whole object must be well-formed; trailing garbage fails. */
            if (skip_ws(p + 1, e) != e) return NULL;
            return found; /* NULL when absent, value ptr when present */
        }
        return NULL;
    }
    return NULL;
}

int json_scan_string(const char *js, size_t len, const char *key, char *out, size_t cap) {
    const char *e = js + len;
    const char *p = find_key(js, len, key);
    if (!p) return -1;
    p = skip_ws(p, e);
    if (p >= e || *p != '"') return -1;
    p++;
    const char *start = p;
    while (p < e) {
        if (*p == '\\') { p += 2; continue; }
        if (*p == '"') break;
        p++;
    }
    if (p >= e) return -1;
    if (json_decode_string(start, (size_t)(p - start), out, cap) == (size_t)-1) return -1;
    return 0;
}

/* Greptile #5: parse the FULL numeric value including fraction and base-10
 * exponent. The old code skipped the exponent suffix, so 2000000000e-10
 * (== 0.2, expired) compared as 2000000000 (accepted). Value computed in
 * long double with bounded scaling loops (<= 60 iterations, saturation at
 * 9e18), strict grammar, and a required value terminator. */
int json_scan_number(const char *js, size_t len, const char *key, long long *out) {
    const char *e = js + len;
    const char *p = find_key(js, len, key);
    if (!p) return -1;
    p = skip_ws(p, e);
    if (p >= e) return -1;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    if (p >= e) return -1;
    long double v = 0;
    if (*p == '0') { p++; }
    else if (*p >= '1' && *p <= '9') {
        while (p < e && *p >= '0' && *p <= '9') { v = v * 10.0L + (*p - '0'); p++; }
    } else return -1;
    if (p < e && *p == '.') {
        p++;
        const char *fs = p;
        long double fdiv = 10.0L;
        int fdig = 0;
        while (p < e && *p >= '0' && *p <= '9' && fdig < 64) {
            v += (*p - '0') / fdiv;
            fdiv *= 10.0L;
            p++;
            fdig++;
        }
        while (p < e && *p >= '0' && *p <= '9') p++; /* ignore beyond 64 frac digits */
        if (p == fs) return -1;
    }
    long e10 = 0;
    if (p < e && (*p == 'e' || *p == 'E')) {
        p++;
        int eneg = 0;
        if (p < e && (*p == '+' || *p == '-')) { eneg = (*p == '-'); p++; }
        const char *es = p;
        long eabs = 0;
        while (p < e && *p >= '0' && *p <= '9') {
            if (eabs < 1000000L) eabs = eabs * 10 + (*p - '0');
            p++;
        }
        if (p == es) return -1;
        e10 = eneg ? -eabs : eabs;
    }
    /* value must terminate cleanly (ws or structural char) */
    if (p < e && *p != ',' && *p != '}' && *p != ']' &&
        *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
        return -1;
    if (e10 > 60 || e10 < -60) {
        /* Huge exponent: decide by decimal order (exact, no overflow).
         * Covers 2000000000e-10 style claims and hostile 1e±999999. */
        if (v == 0) { *out = 0; return 0; }
        int ord = 0, guard = 0;
        long double t = v; /* v > 0 here; sign applied below */
        while (t >= 10.0L && guard++ < 10000) { t /= 10.0L; ord++; }
        guard = 0;
        while (t < 1.0L && guard++ < 10000) { t *= 10.0L; ord--; }
        long tord = (long)ord + e10;
        long double r;
        if (!neg) {
            if (tord > 18) { *out = LLONG_MAX; return 0; }
            if (tord < 0) { *out = 0; return 0; }
            r = t;
            for (long i = 0; i < tord; i++) r *= 10.0L;
            *out = (r >= 9.0e18L) ? LLONG_MAX : (long long)r;
        } else {
            if (tord > 18) { *out = LLONG_MIN; return 0; }
            if (tord < 0) { *out = 0; return 0; }
            r = t;
            for (long i = 0; i < tord; i++) r *= 10.0L;
            r = -r;
            *out = (r <= -9.0e18L) ? LLONG_MIN : (long long)r;
        }
        return 0;
    }
    if (e10 > 0) {
        for (long i = 0; i < e10; i++) {
            v *= 10.0L;
            if (v >= 9.0e18L) break;
        }
        if (v >= 9.0e18L) v = 9.0e18L;
    } else if (e10 < 0) {
        for (long i = 0; i < -e10; i++) v /= 10.0L;
    }
    if (neg) v = -v;
    if (v >= 9.0e18L) *out = LLONG_MAX;
    else if (v <= -9.0e18L) *out = LLONG_MIN;
    else *out = (long long)v;
    return 0;
}

int json_header_is_hs256(const char *js, size_t len) {
    const char *e = js + len;
    const char *p = find_key(js, len, "alg");
    if (!p) return 0;
    p = skip_ws(p, e);
    if (p >= e || *p != '"') return 0;
    p++;
    if ((size_t)(e - p) < 6) return 0;
    if (memcmp(p, "HS256", 5) != 0) return 0;
    if (p[5] != '"') return 0;
    return 1;
}

static int is_trim_c(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

size_t json_trim_inplace(char *s, size_t n) {
    size_t a = 0;
    while (a < n && is_trim_c(s[a])) a++;
    size_t b = n;
    while (b > a && is_trim_c(s[b - 1])) b--;
    size_t m = b - a;
    if (a && m) memmove(s, s + a, m);
    return m;
}

size_t json_charlen(const char *s, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) c++;
    }
    return c;
}

int json_fmt_ts(const char *s, size_t n, char *out, size_t cap) {
    if (cap < 25 || n < 19) return -1;
    memcpy(out, s, 10);
    out[10] = 'T';
    memcpy(out + 11, s + 11, 8);
    out[19] = '.';
    char ms[3] = { '0', '0', '0' };
    if (n > 19 && s[19] == '.') {
        for (int i = 0; i < 3; i++) {
            if ((size_t)(20 + i) >= n) break;
            char d = s[20 + i];
            if (d < '0' || d > '9') break;
            ms[i] = d;
        }
    }
    out[20] = ms[0]; out[21] = ms[1]; out[22] = ms[2];
    out[23] = 'Z'; out[24] = '\0';
    return 0;
}
