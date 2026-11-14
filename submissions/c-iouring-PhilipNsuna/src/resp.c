#include "resp.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void buf_reserve(buf_t *b, size_t need) {
    if (b->len + need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + need) cap *= 2;
    b->p = realloc(b->p, cap);
    if (!b->p) { perror("realloc"); exit(1); }
    b->cap = cap;
}
void buf_append(buf_t *b, const void *s, size_t n) { buf_reserve(b, n); memcpy(b->p + b->len, s, n); b->len += n; }
void buf_int(buf_t *b, int64_t v) {
    char tmp[24]; int n = snprintf(tmp, sizeof tmp, "%lld", (long long)v); buf_append(b, tmp, (size_t)n);
}
void buf_json_string(buf_t *b, const uint8_t *s, size_t n) {
    buf_reserve(b, 6 * n + 2);
    b->len = json_append_string(b->p, b->len, s, n);
}

static const char *status_line(int status) {
    switch (status) {
    case 200: return "HTTP/1.1 200 OK\r\n";
    case 201: return "HTTP/1.1 201 Created\r\n";
    case 400: return "HTTP/1.1 400 Bad Request\r\n";
    case 401: return "HTTP/1.1 401 Unauthorized\r\n";
    case 404: return "HTTP/1.1 404 Not Found\r\n";
    case 413: return "HTTP/1.1 413 Content Too Large\r\n";
    case 500: return "HTTP/1.1 500 Internal Server Error\r\n";
    case 503: return "HTTP/1.1 503 Service Unavailable\r\n";
    }
    return "HTTP/1.1 500 Internal Server Error\r\n";
}

void append_response(buf_t *out, int status, const uint8_t *body, size_t n, bool gzipped, bool close) {
    buf_str(out, status_line(status));
    buf_str(out, "Content-Type: application/json\r\nContent-Length: ");
    buf_int(out, (int64_t)n);
    buf_str(out, "\r\n");
    if (gzipped) buf_str(out, "Content-Encoding: gzip\r\n");
    if (close) buf_str(out, "Connection: close\r\n");
    buf_str(out, "\r\n");
    buf_append(out, body, n);
}

#define PG_EPOCH 946684800LL

static void put2(uint8_t *p, int v) { p[0] = (uint8_t)('0' + v / 10); p[1] = (uint8_t)('0' + v % 10); }

void append_timestamp(buf_t *b, int64_t micros) {
    int64_t secs = micros / 1000000, frac = micros % 1000000;
    if (frac < 0) { frac += 1000000; secs--; }
    time_t t = (time_t)(secs + PG_EPOCH);
    struct tm tm;
    gmtime_r(&t, &tm);
    buf_reserve(b, 24);
    uint8_t *p = b->p + b->len;
    int y = tm.tm_year + 1900;
    p[0] = (uint8_t)('0' + y / 1000); p[1] = (uint8_t)('0' + y / 100 % 10); p[2] = (uint8_t)('0' + y / 10 % 10); p[3] = (uint8_t)('0' + y % 10);
    p[4] = '-'; put2(p + 5, tm.tm_mon + 1); p[7] = '-'; put2(p + 8, tm.tm_mday); p[10] = 'T';
    put2(p + 11, tm.tm_hour); p[13] = ':'; put2(p + 14, tm.tm_min); p[16] = ':'; put2(p + 17, tm.tm_sec); p[19] = '.';
    int ms = (int)(frac / 1000);
    p[20] = (uint8_t)('0' + ms / 100); p[21] = (uint8_t)('0' + ms / 10 % 10); p[22] = (uint8_t)('0' + ms % 10); p[23] = 'Z';
    b->len += 24;
}

void append_post(buf_t *b, int64_t id, const uint8_t *body, size_t blen, int64_t created, const uint8_t *author, size_t alen, int32_t likes) {
    buf_str(b, "{\"id\":"); buf_int(b, id);
    buf_str(b, ",\"body\":"); buf_json_string(b, body, blen);
    buf_str(b, ",\"created_at\":\""); append_timestamp(b, created);
    buf_str(b, "\",\"author\":"); buf_json_string(b, author, alen);
    buf_str(b, ",\"like_count\":"); buf_int(b, likes);
    buf_str(b, "}");
}

void append_health_ok(buf_t *b, int64_t uptime) { buf_str(b, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":"); buf_int(b, uptime); buf_str(b, "}"); }
void append_health_degraded(buf_t *b, const char *err) {
    buf_str(b, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":"); buf_json_string(b, (const uint8_t *)err, strlen(err)); buf_str(b, "}");
}
void append_like(buf_t *b, bool already, int64_t post_id) {
    buf_str(b, "{\"liked\":true,\"already_liked\":"); buf_str(b, already ? "true" : "false");
    buf_str(b, ",\"post_id\":"); buf_int(b, post_id); buf_str(b, "}");
}

int validate_body(const uint8_t *raw, size_t n, uint8_t *scratch, size_t cap, const uint8_t **trimmed, size_t *tlen, const char **msg) {
    if (!json_valid(raw, n)) { *msg = BODY_MALFORMED_JSON; return 400; }
    const uint8_t *v; size_t vl;
    if (json_object_get(raw, n, "body", &v, &vl) != JK_STRING) { *msg = BODY_REQUIRED; return 400; }
    if (vl > cap) { *msg = BODY_TOO_LONG; return 400; }
    long ul = json_unescape(v, vl, scratch);
    if (ul < 0) { *msg = BODY_MALFORMED_JSON; return 400; }
    size_t tl;
    const uint8_t *t = trim_js(scratch, (size_t)ul, &tl);
    if (tl == 0) { *msg = BODY_REQUIRED; return 400; }
    if (utf8_count(t, tl) > 500) { *msg = BODY_TOO_LONG; return 400; }
    *trimmed = t; *tlen = tl;
    return 0;
}
