#include "http.h"
#include <string.h>

static const uint8_t *find_crlfcrlf(const uint8_t *b, size_t n) {
    const uint8_t *p = b, *end = b + n;
    while ((p = memchr(p, '\r', (size_t)(end - p))) != NULL) {
        if (end - p >= 4 && p[1] == '\n' && p[2] == '\r' && p[3] == '\n') return p;
        p++;
    }
    return NULL;
}

static bool eqfold(const uint8_t *b, size_t n, const char *lower) {
    if (strlen(lower) != n) return false;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != (uint8_t)lower[i]) return false;
    }
    return true;
}

static bool containsfold(const uint8_t *b, size_t n, const char *lower) {
    size_t l = strlen(lower);
    for (size_t i = 0; i + l <= n; i++) if (eqfold(b + i, l, lower)) return true;
    return false;
}

static bool digits(const uint8_t *b, size_t n, int64_t *v) {
    if (n == 0 || n > 18) return false;
    int64_t x = 0;
    for (size_t i = 0; i < n; i++) { if (b[i] < '0' || b[i] > '9') return false; x = x * 10 + (b[i] - '0'); }
    *v = x;
    return true;
}

bool parse_id(const uint8_t *s, size_t n, int64_t *out) {
    int64_t v;
    if (!digits(s, n, &v) || v <= 0) return false;
    *out = v;
    return true;
}

static void trimsp(const uint8_t **p, size_t *n) {
    while (*n && (**p == ' ' || **p == '\t')) { (*p)++; (*n)--; }
    while (*n && ((*p)[*n - 1] == ' ' || (*p)[*n - 1] == '\t')) (*n)--;
}

int parse_request(const uint8_t *b, size_t n, request_t *r, size_t *used) {
    const uint8_t *end = find_crlfcrlf(b, n);
    if (!end) return n > MAX_HEADER_BYTES ? P_TOO_LARGE : P_INCOMPLETE;
    size_t head_len = (size_t)(end - b);
    if (head_len > MAX_HEADER_BYTES) return P_TOO_LARGE;
    memset(r, 0, sizeof *r);
    size_t body_start = head_len + 4;
    const uint8_t *p = b, *hend = b + head_len;
    // request line
    const uint8_t *eol = memchr(p, '\n', (size_t)(hend - p));
    const uint8_t *line_end = eol ? eol : hend;
    size_t ll = (size_t)(line_end - p);
    if (ll && p[ll - 1] == '\r') ll--;
    const uint8_t *sp = memchr(p, ' ', ll);
    if (!sp || sp == p) return P_BAD;
    size_t ml = (size_t)(sp - p);
    if (ml == 3 && !memcmp(p, "GET", 3)) r->method = M_GET;
    else if (ml == 4 && !memcmp(p, "POST", 4)) r->method = M_POST;
    else r->method = M_OTHER;
    const uint8_t *rest = sp + 1;
    size_t rl = ll - ml - 1;
    const uint8_t *sp2 = memchr(rest, ' ', rl);
    if (!sp2 || sp2 == rest) return P_BAD;
    r->path = rest; r->path_len = (size_t)(sp2 - rest);
    const uint8_t *q = memchr(r->path, '?', r->path_len);
    if (q) r->path_len = (size_t)(q - r->path);
    const uint8_t *ver = sp2 + 1;
    size_t vl = rl - r->path_len - 1;
    if (vl != 8 || memcmp(ver, "HTTP/1.", 7)) return P_BAD;
    bool http10 = ver[7] == '0';
    r->close = http10;
    p = eol ? eol + 1 : hend;
    int64_t clen = 0;
    bool chunked = false;
    while (p < hend) {
        eol = memchr(p, '\n', (size_t)(hend - p));
        line_end = eol ? eol : hend;
        ll = (size_t)(line_end - p);
        if (ll && p[ll - 1] == '\r') ll--;
        if (ll) {
            const uint8_t *colon = memchr(p, ':', ll);
            if (!colon || colon == p) return P_BAD;
            size_t nl = (size_t)(colon - p);
            const uint8_t *val = colon + 1; size_t vlen = ll - nl - 1;
            trimsp(&val, &vlen);
            switch (nl) {
            case 14: if (eqfold(p, nl, "content-length")) { if (!digits(val, vlen, &clen)) return P_BAD; if (clen > MAX_BODY_BYTES) return P_TOO_LARGE; } break;
            case 13: if (eqfold(p, nl, "authorization")) { r->auth = val; r->auth_len = vlen; } break;
            case 15: if (eqfold(p, nl, "accept-encoding")) r->gzip = containsfold(val, vlen, "gzip"); break;
            case 10: if (eqfold(p, nl, "connection")) { if (containsfold(val, vlen, "close")) r->close = true; else if (http10 && containsfold(val, vlen, "keep-alive")) r->close = false; } break;
            case 17: if (eqfold(p, nl, "transfer-encoding")) chunked = containsfold(val, vlen, "chunked"); break;
            }
        }
        p = eol ? eol + 1 : hend;
    }
    if (chunked) return P_BAD;
    if (n < body_start + (size_t)clen) return P_INCOMPLETE;
    r->body = b + body_start; r->body_len = (size_t)clen;
    *used = body_start + (size_t)clen;
    return P_OK;
}
