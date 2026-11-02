#include "http.h"
#include "net.h"
#include "json.h"
#include "jwt.h"
#include "pg.h"
#include <libpq-fe.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <libdeflate.h>
#include "picohttpparser.h"
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

/* Thread-local scratch for decoded POST bodies (raw cap 16K + slack). */
static _Thread_local char t_dec[17408];

static int ci_eq(const char *a, size_t al, const char *b) {
    size_t bl = strlen(b);
    if (al != bl) return 0;
    return strncasecmp(a, b, bl) == 0;
}

/* Pass 18: HTTP/1.x request-line + headers via vendored picohttpparser
 * (pinned h2o commit, static; parse-in-place, zero-copy: method/path/header
 * values point into buf, no NUL termination, no per-request malloc).
 * Wire semantics match the hand parser it replaces: routes, auth-before-id,
 * strict id validation, 16K header / 16K body caps, query/trailing-slash
 * 404s, Connection/Accept-Encoding handling.
 * Known fail-closed deltas (all untested edges): a non-HTTP/1.x version
 * token (e.g. HTTP/2.0) now fails the parse (close) instead of defaulting
 * to 1.1; >64 headers fail instead of scanning on; SP before the header
 * colon fails instead of trimming. Transfer-Encoding stays ignored
 * (Content-Length rules), exactly as before. */
#define PICO_MAX_HEADERS 64

int http_parse(const char *buf, size_t len, http_req_t *out) {
    memset(out, 0, sizeof *out);
    out->method = -1; out->content_length = -1;
    out->ver_major = 1; out->ver_minor = 1;
    const char *method = NULL, *path = NULL;
    size_t method_len = 0, path_len = 0;
    int minor = 1;
    struct phr_header hdrs[PICO_MAX_HEADERS];
    size_t nhdrs = PICO_MAX_HEADERS;
    int pret = phr_parse_request(buf, len, &method, &method_len,
                                 &path, &path_len, &minor,
                                 hdrs, &nhdrs, 0);
    if (pret == -2) {
        /* Incomplete: wait for more, but never past the 32K slab cap
         * (the conn path closes past HDR_CAP+BODY_CAP anyway). */
        if (len > 16384 + 16384) return -1;
        return 0;
    }
    if (pret < 0) return -1;             /* malformed: caller closes */
    if ((size_t)pret > 16384) return -1; /* header cap 16K */
    if (method_len == 3 && memcmp(method, "GET", 3) == 0) out->method = 0;
    else if (method_len == 4 && memcmp(method, "POST", 4) == 0) out->method = 1;
    else out->method = -1;               /* unknown method -> 404 below */
    out->target = path;
    out->target_len = path_len;
    out->ver_major = 1;
    out->ver_minor = (minor == 0) ? 0 : 1;
    out->header_len = (size_t)pret;
    for (size_t i = 0; i < nhdrs; i++) {
        const char *nm = hdrs[i].name;
        size_t nl = hdrs[i].name_len;
        const char *vs = hdrs[i].value;
        size_t vl = hdrs[i].value_len;
        if (!nm) continue; /* folded continuation line: ignore as before */
        while (nl && (nm[nl-1] == ' ' || nm[nl-1] == '\t')) nl--;
        while (vl && (*vs == ' ' || *vs == '\t')) { vs++; vl--; }
        while (vl && (vs[vl-1] == ' ' || vs[vl-1] == '\t')) vl--;
        if (ci_eq(nm, nl, "content-length")) {
            char tmp[32]; size_t c = vl < sizeof tmp - 1 ? vl : sizeof tmp - 1;
            memcpy(tmp, vs, c); tmp[c] = '\0';
            long v = -1;
            if (c > 0) {
                char *e = NULL; v = strtol(tmp, &e, 10);
                if (e == tmp) v = -1;
            }
            out->content_length = v;
        } else if (ci_eq(nm, nl, "authorization")) {
            out->auth = vs; out->auth_len = vl;
        } else if (ci_eq(nm, nl, "connection")) {
            out->connection = vs; out->connection_len = vl;
        } else if (ci_eq(nm, nl, "accept-encoding")) {
            /* Pass 7: feed-first gzip. Token search, case-insensitive,
             * separators are comma/space/tab/semicolon. */
            for (size_t k = 0; k + 4 <= vl; k++) {
                if (((vs[k] | 32) == 'g') && ((vs[k+1] | 32) == 'z') &&
                    ((vs[k+2] | 32) == 'i') && ((vs[k+3] | 32) == 'p')) {
                    char pb = (k == 0) ? ',' : vs[k-1];
                    char nb = (k + 4 >= vl) ? ',' : vs[k+4];
                    int pok = (pb == ',' || pb == ' ' || pb == '\t');
                    int nok = (nb == ',' || nb == ' ' || nb == '\t' || nb == ';');
                    if (pok && nok) { out->accept_gzip = 1; break; }
                }
            }
        } else if (ci_eq(nm, nl, "host")) {
            /* scanned only. */
        }
    }
    long cl = out->content_length;
    if (cl < 0) cl = 0;
    if (cl > 16384) return -2; /* body too large: close (out of scope) */
    if (len - (size_t)pret < (size_t)cl) return 0; /* need body */
    out->body = buf + pret;
    out->body_len = (size_t)cl;
    return (int)((size_t)pret + (size_t)cl);
}

int http_send(int fd, const char *hdr, size_t hlen, const char *body, size_t blen) {
    /* Header + body coalesced into one sendmsg (single syscall typically,
     * MSG_NOSIGNAL). Same contract as before: any transport error (incl.
     * EAGAIN on the nonblocking fd) fails and the caller closes. Kept for
     * compat; the conn path below (send_via_conn) is used instead. */
    struct iovec iv[2];
    iv[0].iov_base = (void *)hdr; iv[0].iov_len = hlen;
    iv[1].iov_base = (void *)body; iv[1].iov_len = blen;
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = iv;
    mh.msg_iovlen = (blen > 0) ? 2 : 1;
    size_t total = hlen + blen, off = 0;
    while (off < total) {
        ssize_t r = sendmsg(fd, &mh, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1;
        off += (size_t)r;
        size_t left = (size_t)r;
        while (left > 0 && mh.msg_iovlen > 0) {
            if (left >= mh.msg_iov->iov_len) {
                left -= mh.msg_iov->iov_len;
                mh.msg_iov++;
                mh.msg_iovlen--;
            } else {
                mh.msg_iov->iov_base = (char *)mh.msg_iov->iov_base + left;
                mh.msg_iov->iov_len -= left;
                left = 0;
            }
        }
    }
    return 0;
}

/* Stash cap for an unsent response: a slow client can hold at most this many
 * bytes before we close instead of growing (bounds per-conn memory). */
#define WSTASH_CAP (256 * 1024)

/* Pass 9 conn-aware send: still a single coalesced sendmsg on the fast
 * path. On EAGAIN (sndbuf full under load) the remainder is stashed in the
 * conn and the fd is EPOLLOUT re-armed instead of closing: the event loop
 * drains it before parsing more requests, preserving response order.
 * Returns 0 (sent or stashed; conn stays open), -1 on transport error or
 * stash overflow (caller must close). */
static int send_via_conn(struct conn_hot *c, const char *hdr, size_t hlen,
                         const char *body, size_t blen) {
    struct iovec iv[2];
    iv[0].iov_base = (void *)hdr; iv[0].iov_len = hlen;
    iv[1].iov_base = (void *)body; iv[1].iov_len = blen;
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = iv;
    mh.msg_iovlen = (blen > 0) ? 2 : 1;
    size_t total = hlen + blen, off = 0;
    while (off < total) {
        ssize_t r = sendmsg(c->fd, &mh, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
            break; /* stash the remainder below */
        }
        if (r == 0) return -1;
        off += (size_t)r;
        size_t left = (size_t)r;
        while (left > 0 && mh.msg_iovlen > 0) {
            if (left >= mh.msg_iov->iov_len) {
                left -= mh.msg_iov->iov_len;
                mh.msg_iov++;
                mh.msg_iovlen--;
            } else {
                mh.msg_iov->iov_base = (char *)mh.msg_iov->iov_base + left;
                mh.msg_iov->iov_len -= left;
                left = 0;
            }
        }
    }
    if (off == total) return 0;
    /* Would-block: stash [off,total) of the hdr+body concat, in order.
     * A pre-existing stash (pipelined stall) is appended to, preserving
     * response order; it is drained before any further parsing. */
    size_t rem = total - off;
    if (c->cold->wlen + rem > WSTASH_CAP) return -1;
    char *nb;
    if (c->cold->wbuf) {
        nb = realloc(c->cold->wbuf, c->cold->wlen + rem);
        if (!nb) return -1;
        c->cold->wbuf = nb;
    } else {
        nb = malloc(rem);
        if (!nb) return -1;
        c->cold->wbuf = nb; c->cold->wlen = 0; c->cold->woff = 0;
    }
    size_t copied = 0;
    if (off < hlen) {
        size_t k = hlen - off;
        memcpy(c->cold->wbuf + c->cold->wlen, hdr + off, k);
        copied = k;
    }
    {
        size_t boff = (off > hlen) ? off - hlen : 0;
        size_t k = blen > boff ? blen - boff : 0;
        if (k) memcpy(c->cold->wbuf + c->cold->wlen + copied, body + boff, k);
    }
    c->cold->wlen += rem;
    if (net_arm_out(c) != 0) return -1;
    return 0;
}

/* Route helpers */
static int target_eq(const http_req_t *rq, const char *s) {
    size_t n = strlen(s);
    if (rq->target_len != n) return 0;
    return memcmp(rq->target, s, n) == 0;
}

/* Strict positive-integer id: ^[1-9][0-9]*$, 1..15 digits. 0 else. */
static int valid_id(const char *s, size_t n, unsigned long long *v) {
    if (n == 0 || n > 15 || s[0] == '0') return 0;
    unsigned long long x = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        x = x * 10 + (unsigned)(s[i] - '0');
    }
    if (x == 0 || x > 9223372036854775807ULL) return 0;
    if (v) *v = x;
    return 1;
}

/* Auth check for protected routes. 0 ok (sub/user filled), -1 + 401 sent. */
static void send_resp(struct conn_hot *c, const http_req_t *rq, int code, const char *reason,
                      const char *body, size_t blen, int force_close);
static int check_auth(struct conn_hot *c, const http_req_t *rq,
                      char *sub, size_t subcap, char *user, size_t usercap) {
    const char *emsg = NULL;
    if (unlikely(!rq->auth || rq->auth_len <= 7 || memcmp(rq->auth, "Bearer ", 7) != 0)) {
        emsg = JERR_MISSING_BEARER;
        send_resp(c, rq, 401, "Unauthorized", emsg, strlen(emsg), 0);
        return -1;
    }
    long long uid = 0;
    int r = jwt_verify(rq->auth + 7, rq->auth_len - 7, jwt_secret(),
                       sub, subcap, &uid, user, usercap);
    if (r == JWT_PAYLOAD) emsg = JERR_INVALID_PAYLOAD;
    else if (r != JWT_OK) emsg = JERR_INVALID_TOKEN;
    if (unlikely(emsg)) {
        send_resp(c, rq, 401, "Unauthorized", emsg, strlen(emsg), 0);
        return -1;
    }
    return 0;
}

static int wants_close(const http_req_t *rq) {
    if (rq->connection && rq->connection_len) {
        char tmp[64]; size_t c = rq->connection_len < sizeof tmp - 1 ? rq->connection_len : sizeof tmp - 1;
        for (size_t i = 0; i < c; i++) tmp[i] = (char)tolower((unsigned char)rq->connection[i]);
        tmp[c] = '\0';
        if (strstr(tmp, "close")) return 1;
        return 0;
    }
    if (rq->ver_major == 1 && rq->ver_minor == 0) return 1; /* HTTP/1.0 default close */
    return 0;
}

/* Prebuilt status lines: no snprintf/printf on the hot path. */
static const char ST200[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ";
static const char ST201[] = "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: ";
static const char ST400[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: ";
static const char ST401[] = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: ";
static const char ST404[] = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: ";
static const char ST500[] = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: ";
static const char ST503[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Length: ";
static const char CONN_HDR_CLOSE[] = "\r\nConnection: close\r\n\r\n";
static const char CONN_HDR_KEEP[] = "\r\nConnection: keep-alive\r\n\r\n";

/* Decimal append without locale/printf overhead. Returns end pointer. */
static char *append_u64(char *p, size_t v) {
    char tmp[20];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    }
    while (n > 0) *p++ = tmp[--n];
    return p;
}

/* ---- Pass 7: feed-first gzip (gzip framing);
 * Pass 15: vendored libdeflate L1 replaces system zlib.
 * Pass 22: dual auto-tier L0 (store) + L1 per thread. Routing is by size
 * only, never by route: ilen >= GZIP_L0_THRESH (large feeds, CPU-bound at
 * ~28% on-CPU gzip) takes L0 at ~1/3 the CPU for a worse ratio; smaller
 * bodies keep L1 (best ratio, network matters, small CPU either way).
 * Single posts never reach gzip (<=650 B < GZIP_MIN_POST), so in practice
 * the dual tier only affects the feed. Every request with AE compresses
 * fresh (no memoization across writes, Rule 7); the tier choice never
 * skips gzip, it only picks the level. Any failure falls back to identity
 * in the caller, byte-exact. ---- */

#define GZIP_MIN_POST 1024
/* Pass 22: feed bodies run 3-5 KiB typical; >= 4 KiB goes L0 (store). */
#define GZIP_L0_THRESH 4096
/* Worst-case gzip input is t_feed (128 KiB, pg.c); the slab below is
 * pre-sized once per level from libdeflate_gzip_compress_bound() for that
 * class and then reused every request (no per-request sizing). */
#define GZ_MAX_IN 131072
/* Worst-case gzip expansion of 128 KiB stays well under GZ_CAP for both
 * levels, so a 0 return (output did not fit) falls through to identity. */
#define GZ_CAP 139264

static _Thread_local struct libdeflate_compressor *t_ldc_l1 = NULL;
static _Thread_local struct libdeflate_compressor *t_ldc_l0 = NULL;
static _Thread_local unsigned char t_gz[GZ_CAP];
/* Worst-case bounds sampled once per level (0 = not sampled yet). */
static _Thread_local size_t t_gz_bound_l1 = 0;
static _Thread_local size_t t_gz_bound_l0 = 0;

/* Pick the tier compressor for ilen, allocating lazily once per thread.
 * Returns NULL only when that tier's allocator fails (caller tries the
 * other tier, then identity). Never returns NULL for both when malloc
 * works: L1 alloc failure with L0 already live still serves via L0. */
static struct libdeflate_compressor *gzip_comp_for(size_t ilen) {
    if (ilen >= GZIP_L0_THRESH) {
        if (unlikely(t_ldc_l0 == NULL)) {
            t_ldc_l0 = libdeflate_alloc_compressor(0);
            if (t_ldc_l0 != NULL)
                t_gz_bound_l0 = libdeflate_gzip_compress_bound(t_ldc_l0, GZ_MAX_IN);
        }
        if (t_ldc_l0 != NULL)
            return t_ldc_l0;
    }
    if (unlikely(t_ldc_l1 == NULL)) {
        t_ldc_l1 = libdeflate_alloc_compressor(1);
        if (t_ldc_l1 != NULL)
            t_gz_bound_l1 = libdeflate_gzip_compress_bound(t_ldc_l1, GZ_MAX_IN);
    }
    return t_ldc_l1;
}

/* Compress in[0..ilen) into thread-local t_gz. 0 ok (out+olen set), -1 on
 * failure (caller falls back to identity, byte-exact). No per-request malloc:
 * two compressors (L0+L1) per thread, allocated once and reused; the slab
 * is bounds-checked against the once-sampled worst case for its tier. */
static int gzip_compress(const char *in, size_t ilen,
                         const unsigned char **out, size_t *olen) {
    struct libdeflate_compressor *c = gzip_comp_for(ilen);
    if (c == NULL)
        return -1;
    size_t bound = (c == t_ldc_l0) ? t_gz_bound_l0 : t_gz_bound_l1;
    /* Fail closed to identity when the input exceeds the pre-sized class
     * or the tier bound does not fit the slab (defensive; neither fires:
     * feed input caps at GZ_MAX_IN and both bounds sit well under GZ_CAP). */
    if (unlikely(ilen > GZ_MAX_IN || bound == 0 || bound > sizeof t_gz))
        return -1;
    size_t n = libdeflate_gzip_compress(c, in, ilen, t_gz, sizeof t_gz);
    if (n == 0)
        return -1;
    *out = t_gz;
    *olen = n;
    return 0;
}

static const char ENC_GZIP[] = "\r\nContent-Encoding: gzip";

static void send_resp_ex(struct conn_hot *c, const http_req_t *rq, int code, const char *reason,
                         const char *body, size_t blen, int force_close, int allow_gzip) {
    const char *st = NULL;
    size_t sl = 0;
    switch (code) {
    case 200: st = ST200; sl = sizeof ST200 - 1; break;
    case 201: st = ST201; sl = sizeof ST201 - 1; break;
    case 400: st = ST400; sl = sizeof ST400 - 1; break;
    case 401: st = ST401; sl = sizeof ST401 - 1; break;
    case 404: st = ST404; sl = sizeof ST404 - 1; break;
    case 500: st = ST500; sl = sizeof ST500 - 1; break;
    case 503: st = ST503; sl = sizeof ST503 - 1; break;
    default: break;
    }
    char hdr[256];
    int close_it = force_close || wants_close(rq) || c->req_count >= 999;
    /* Greptile #11: a Connection:close response that EAGAIN-stashes must be
     * sent to the last byte BEFORE the close (old code freed the conn with
     * the stash unsent -> truncation; and a drained-then-idle close verdict
     * could be lost). Mark close_after_drain and stay open; drain_pending
     * closes once the stash is empty. */
    if (likely(st != NULL)) {
        /* Gzip fast path: only when the route allows it AND the client
         * asked. Any compression failure falls through to identity. */
        if (allow_gzip && rq->accept_gzip && body && blen > 0) {
            const unsigned char *gz = NULL;
            size_t gzlen = 0;
            if (gzip_compress(body, blen, &gz, &gzlen) == 0) {
                char *p = hdr;
                memcpy(p, st, sl); p += sl;
                p = append_u64(p, gzlen);
                memcpy(p, ENC_GZIP, sizeof ENC_GZIP - 1); p += sizeof ENC_GZIP - 1;
                const char *ce = close_it ? CONN_HDR_CLOSE : CONN_HDR_KEEP;
                size_t cl = close_it ? sizeof CONN_HDR_CLOSE - 1 : sizeof CONN_HDR_KEEP - 1;
                memcpy(p, ce, cl); p += cl;
                if (send_via_conn(c, hdr, (size_t)(p - hdr), (const char *)gz, gzlen) != 0)
                    c->keep_alive = 0;
                else if (close_it && c->cold->wbuf) {
                    c->cold->close_after_drain = 1;
                    c->keep_alive = 1;
                } else
                    c->keep_alive = !close_it;
                return;
            }
        }
        char *p = hdr;
        memcpy(p, st, sl); p += sl;
        p = append_u64(p, blen);
        const char *ce = close_it ? CONN_HDR_CLOSE : CONN_HDR_KEEP;
        size_t cl = close_it ? sizeof CONN_HDR_CLOSE - 1 : sizeof CONN_HDR_KEEP - 1;
        memcpy(p, ce, cl); p += cl;
        if (send_via_conn(c, hdr, (size_t)(p - hdr), body, blen) != 0)
            c->keep_alive = 0;
        else if (close_it && c->cold->wbuf) {
            c->cold->close_after_drain = 1;
            c->keep_alive = 1;
        } else
            c->keep_alive = !close_it;
        return;
    }
    /* Unknown code (defensive; no current caller): printf fallback. */
    (void)reason;
    const char *conn = close_it ? "close" : "keep-alive";
    int hlen = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: %s\r\n\r\n",
        code, reason ? reason : "", blen, conn);
    if (hlen < 0 || (size_t)hlen >= sizeof hdr) return;
    if (send_via_conn(c, hdr, (size_t)hlen, body, blen) != 0)
        c->keep_alive = 0;
    else
        c->keep_alive = !close_it;
}

/* Identity wrapper: errors, health, like, create and unknown routes stay
 * uncompressed so golden-exact fallback is preserved. */
static void send_resp(struct conn_hot *c, const http_req_t *rq, int code, const char *reason,
                      const char *body, size_t blen, int force_close) {
    send_resp_ex(c, rq, code, reason, body, blen, force_close, 0);
}

/* ---- minimal JSON body parser for POST /posts ---- */

static const char *jws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* Validate a JSON string inner payload (escapes legal, no raw controls,
 * surrogate pairs well-formed). 1 valid, 0 invalid. Mirrors json.c's
 * checker so skipped/ignored strings are held to the decoded grammar. */
static int hstr_inner_valid(const char *s, size_t n) {
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
        if (c < 0x20) return 0;
        i++;
    }
    return 1;
}

/* String at *p == '"': inner in [s,n), returns past closing quote or NULL.
 * Greptile #2: raw control bytes (incl NUL) inside a string are malformed. */
static const char *jstr_end(const char *p, const char *e, const char **s, size_t *n) {
    const char *q = p + 1;
    while (q < e) {
        if (*q == '\\') { q += 2; continue; }
        if ((unsigned char)*q < 0x20) return NULL;
        if (*q == '"') { *s = p + 1; *n = (size_t)(q - (p + 1)); return q + 1; }
        q++;
    }
    return NULL;
}

/* Skip one JSON value; returns past it or NULL.
 * Greptile #2: every skipped string (keys at any depth, ignored values) is
 * escape/controls-validated, so {"body":"ok","extra":"\q"} is malformed
 * instead of silently creating a post. */
static const char *jskip(const char *p, const char *e) {
    p = jws(p, e);
    if (p >= e) return NULL;
    if (*p == '"') {
        const char *s; size_t n;
        const char *np = jstr_end(p, e, &s, &n);
        if (!np) return NULL;
        if (!hstr_inner_valid(s, n)) return NULL;
        return np;
    }
    if (*p == '{') {
        p++;
        p = jws(p, e);
        if (p < e && *p == '}') return p + 1;
        while (1) {
            const char *s; size_t n;
            p = jws(p, e);
            if (p >= e || *p != '"') return NULL;
            p = jstr_end(p, e, &s, &n);
            if (!p) return NULL;
            if (!hstr_inner_valid(s, n)) return NULL;
            p = jws(p, e);
            if (p >= e || *p != ':') return NULL;
            p = jskip(p + 1, e);
            if (!p) return NULL;
            p = jws(p, e);
            if (p >= e) return NULL;
            if (*p == ',') { p++; continue; }
            if (*p == '}') return p + 1;
            return NULL;
        }
    }
    if (*p == '[') {
        p++;
        p = jws(p, e);
        if (p < e && *p == ']') return p + 1;
        while (1) {
            p = jskip(p, e);
            if (!p) return NULL;
            p = jws(p, e);
            if (p >= e) return NULL;
            if (*p == ',') { p++; continue; }
            if (*p == ']') return p + 1;
            return NULL;
        }
    }
    /* literal: true / false / null / number */
    const char *q = p;
    while (q < e && *q != ',' && *q != '}' && *q != ']' &&
           *q != ' ' && *q != '\t' && *q != '\n' && *q != '\r') q++;
    if (q == p) return NULL;
    size_t L = (size_t)(q - p);
    if (L == 4 && (memcmp(p, "true", 4) == 0 || memcmp(p, "null", 4) == 0)) return q;
    if (L == 5 && memcmp(p, "false", 5) == 0) return q;
    /* number: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)? */
    {
        const char *r = p;
        if (r < q && *r == '-') r++;
        if (r >= q) return NULL;
        if (*r == '0') { r++; }
        else if (*r >= '1' && *r <= '9') { while (r < q && *r >= '0' && *r <= '9') r++; }
        else return NULL;
        if (r < q && *r == '.') {
            r++;
            const char *fs = r;
            while (r < q && *r >= '0' && *r <= '9') r++;
            if (r == fs) return NULL;
        }
        if (r < q && (*r == 'e' || *r == 'E')) {
            r++;
            if (r < q && (*r == '+' || *r == '-')) r++;
            const char *es = r;
            while (r < q && *r >= '0' && *r <= '9') r++;
            if (r == es) return NULL;
        }
        if (r != q) return NULL;
    }
    return q;
}

/* Extract top-level "body" member.
 * Returns -1 malformed, 0 no-body-key, 1 found (*is_string; decoded in dec). */
static int extract_body(const char *b, size_t n, char *dec, size_t deccap,
                        size_t *declen, int *is_string) {
    const char *e = b + n;
    const char *p = jws(b, e);
    if (p >= e || *p != '{') return -1;
    p++;
    p = jws(p, e);
    if (p < e && *p == '}') {
        p = jws(p + 1, e);
        return (p == e) ? 0 : -1;
    }
    int found = 0;
    while (1) {
        const char *ks; size_t kn;
        p = jws(p, e);
        if (p >= e || *p != '"') return -1;
        const char *np = jstr_end(p, e, &ks, &kn);
        if (!np) return -1;
        p = jws(np, e);
        if (p >= e || *p != ':') return -1;
        p = jws(p + 1, e);
        if (p >= e) return -1;
        /* Greptile #1: keys are escape-decoded before compare, so
         * {"\u0062ody":"hello"} matches "body". Greptile #2: every key and
         * every ignored value is validated (bad escapes / raw controls ->
         * malformed), so {"body":"ok","extra":"\q"} cannot create a post. */
        if (!hstr_inner_valid(ks, kn)) return -1;
        int is_body = 0;
        if (kn == 4 && memcmp(ks, "body", 4) == 0) {
            is_body = 1; /* fast path: no escapes */
        } else if (memchr(ks, '\\', kn) != NULL && kn < 64) {
            char kd[64];
            if (json_decode_string(ks, kn, kd, sizeof kd) == (size_t)-1) return -1;
            if (strcmp(kd, "body") == 0) is_body = 1;
        }
        if (*p == '"') {
            const char *vs; size_t vn;
            np = jstr_end(p, e, &vs, &vn);
            if (!np) return -1;
            if (!hstr_inner_valid(vs, vn)) return -1;
            if (is_body && !found) {
                size_t dl = json_decode_string(vs, vn, dec, deccap);
                if (dl == (size_t)-1) return -1;
                *declen = dl;
                *is_string = 1;
                found = 1;
            }
            p = np;
        } else {
            if (is_body && !found) { *is_string = 0; found = 1; }
            p = jskip(p, e);
            if (!p) return -1;
        }
        p = jws(p, e);
        if (p >= e) return -1;
        if (*p == ',') { p++; continue; }
        if (*p == '}') { p++; break; }
        return -1;
    }
    p = jws(p, e);
    if (p != e) return -1;
    return found ? 1 : 0;
}

/* 404-mismatch tripwire (must stay silent in production: every logged line
 * is a lost post under load). Capped per thread so a regression cannot
 * flood the log; validation greps for POST404 and expects zero lines. */
static _Thread_local unsigned t_post404_n = 0;
static void log_post404(const char *where, const char *want_id, PGresult *res) {
    if (t_post404_n >= 16) return;
    t_post404_n++;
    int nt = res ? PQntuples(res) : -1;
    int st = res ? (int)PQresultStatus(res) : -1;
    fprintf(stderr, "POST404 %s want=%s ntuples=%d status=%d\n",
            where, want_id ? want_id : "-", nt, st);
}

void http_pending_free(struct pg_pending *p) {
    if (!p) return;
    free(p->p0); p->p0 = NULL;
    free(p->p1); p->p1 = NULL;
    free(p->author); p->author = NULL;
    free(p->conn_hdr); p->conn_hdr = NULL;
    p->route = PG_ROUTE_NONE;
    p->slot = -1;
    p->sent = 0;
    p->npar = 0;
    p->body_len = 0;
    p->idv = 0;
    p->ver_major = 1; p->ver_minor = 1;
    p->conn_hdr_len = 0;
    p->uptime_s = 0;
    p->accept_gzip = 0;
    p->pg_gen = 0;
    /* NOTE: wq_owner/wq_next/wq_prev are NOT cleared here. The waiter node
     * stays listed until net.c unlinks it (pg_waiter_del_node, called by the
     * resume-completion and conn-free paths with the links intact). Clearing
     * the node without unlinking would leave a zombie in the per-slot list:
     * on malloc address reuse + re-defer the stale head pointer self-loops
     * the list and hangs the completion poll forever. */
}

static char *dup_str(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *d = malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n + 1);
    return d;
}

static const char *stmt_for_route(int route) {
    switch (route) {
    case PG_ROUTE_HEALTH: return "q_health";
    case PG_ROUTE_FEED: return "q_feed";
    case PG_ROUTE_POST: return "q_post";
    case PG_ROUTE_MKPOST: return "q_mkpost";
    case PG_ROUTE_LIKE: return "q_like";
    default: return NULL;
    }
}

/* Overload fail-safe: max queued+in-flight waiters per PG slot on one
 * worker. Steady state holds single digits per slot (Little's law: ~6k
 * rps/worker x ~2ms PG latency); even a 500ms PG stall at saturation stays
 * two orders of magnitude below this. Past it the pool is saturated and
 * queueing more only grows O(waiters) completion scans + per-waiter heap
 * dups, so defer fails fast (-1: existing blocking-fallback path answers
 * 500/503) instead of queueing without bound. Pure overload behavior;
 * normal-path verdicts are unchanged. */
#define PG_WAIT_MAX_PER_SLOT 4096
/* Pass 21: total live PG_WAIT waiters per worker. The per-slot bound above
 * allows 32x4096 in theory; this caps the summed intrusive-list length so
 * every completion stays O(actual waiters) with a hard 4096 ceiling.
 * Steady state holds single digits per slot, so normal-path verdicts are
 * unchanged; only true pool saturation fails fast here. */
#define PG_WAIT_MAX 4096

/* Stash the request for PG_WAIT and kick the async send.
 * Returns HTTP_DEFERRED when stashed (caller must arm the PG socket and
 * stop pipelining), or -1 when the caller should use the blocking fallback
 * (send error, no PQsocket, malloc failure, or the slot's waiter queue is
 * past the overload bound). */
static int http_defer(struct conn_hot *c, const http_req_t *rq, long uptime_s,
                      int route, const char *stmt, int npar,
                      const char *p0, const char *p1,
                      const char *author, size_t body_len,
                      unsigned long long idv) {
    char *cp0 = NULL, *cp1 = NULL, *ca = NULL, *ch = NULL;
    if (npar > 0 && p0) { cp0 = dup_str(p0); if (!cp0) return -1; }
    if (npar > 1 && p1) { cp1 = dup_str(p1); if (!cp1) { free(cp0); return -1; } }
    if (author) { ca = dup_str(author); if (!ca) { free(cp0); free(cp1); return -1; } }
    if (rq->connection && rq->connection_len) {
        size_t cn = rq->connection_len;
        ch = malloc(cn + 1);
        if (!ch) { free(cp0); free(cp1); free(ca); return -1; }
        memcpy(ch, rq->connection, cn);
        ch[cn] = '\0';
    }
    /* Pass 17: R/W-split claim. Reads (health/feed/post) take R slots,
     * writes (mkpost/like) take W slots with their own Sync/autocommit.
     * Each write is committed before its response (autocommit); no
     * shared-txn group commit. */
    int slot = pg_claim_slot_for_route(route);
    if (slot < 0 || slot >= PG_POOL_N || net_waiter_count(slot) >= PG_WAIT_MAX_PER_SLOT ||
        net_waiter_total() >= PG_WAIT_MAX) {
        free(cp0); free(cp1); free(ca); free(ch);
        return -1;
    }
    /* Send with the heap copies (same bytes the stash keeps): the caller's
     * p0/p1 may be stack (ids[]) or thread-local (t_dec) buffers that the
     * next request reuses. libpq copies synchronously, but the heap copies
     * remove any lifetime question entirely.
     * Pass 19 (Greptile #13 hardening): every in-flight slot has an explicit
     * owner (this conn) plus a claim generation. pg_poll refuses to let a
     * different owner consume the result, so a recycled/shared slot can
     * never cross-wire a foreign result (the 404 class). */
    static _Thread_local unsigned t_pg_gen = 0;
    unsigned g = ++t_pg_gen;
    if (g == 0) g = ++t_pg_gen; /* gen 0 = unlinked; never hand it out */
    const char *par[2] = { cp0, cp1 };
    int rc = pg_send_async(slot, stmt, npar, npar ? par : NULL, c, g);
    int fd = pg_socket(slot);
    if (fd < 0 || rc == PG_ASYNC_ERR) {
        free(cp0); free(cp1); free(ca); free(ch);
        return -1;
    }
    c->state = CONN_PG_WAIT;
    c->cold->pend.route = route;
    c->cold->pend.slot = slot;
    c->cold->pend.sent = (rc == PG_ASYNC_OK) ? 1 : 0;
    c->cold->pend.npar = npar;
    c->cold->pend.p0 = cp0;
    c->cold->pend.p1 = cp1;
    c->cold->pend.author = ca;
    c->cold->pend.body_len = body_len;
    c->cold->pend.idv = idv;
    c->cold->pend.ver_major = rq->ver_major;
    c->cold->pend.ver_minor = rq->ver_minor;
    c->cold->pend.conn_hdr = ch;
    c->cold->pend.conn_hdr_len = rq->connection_len;
    c->cold->pend.uptime_s = uptime_s;
    c->cold->pend.accept_gzip = rq->accept_gzip ? 1 : 0;
    c->cold->pend.pg_gen = g;
    return HTTP_DEFERRED;
}

int http_respond(struct conn_hot *c, const http_req_t *rq, long uptime_s) {
    /* Validation order: route -> auth -> id -> body -> DB. */
    if (likely(rq->method == 0) && target_eq(rq, "/health")) {
        int d = http_defer(c, rq, uptime_s, PG_ROUTE_HEALTH, "q_health", 0,
                           NULL, NULL, NULL, 0, 0);
        if (d == HTTP_DEFERRED) return HTTP_DEFERRED;
        char err[512]; err[0] = '\0';
        char body[1024];
        size_t bl;
        if (pg_health_check(err, sizeof err) == 0) {
            bl = json_render_health(body, sizeof body, uptime_s);
            send_resp(c, rq, 200, "OK", body, bl, 0);
        } else {
            static const char degraded[] = "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"db unreachable\"}";
            if (!err[0]) snprintf(err, sizeof err, "db unreachable");
            fprintf(stderr, "health degraded: %s\n", err);
            send_resp(c, rq, 503, "Service Unavailable", degraded, sizeof degraded - 1, 0);
        }
        return c->keep_alive;
    }
    if (likely(rq->method == 0) && target_eq(rq, "/feed")) {
        int d = http_defer(c, rq, uptime_s, PG_ROUTE_FEED, "q_feed", 0,
                           NULL, NULL, NULL, 0, 0);
        if (d == HTTP_DEFERRED) return HTTP_DEFERRED;
        const char *body = NULL;
        size_t bl = 0;
        char err[512];
        if (pg_feed(&body, &bl, err, sizeof err) == 0) {
            send_resp_ex(c, rq, 200, "OK", body, bl, 0, 1);
        } else {
            send_resp(c, rq, 500, "Internal Server Error", JERR_INTERNAL, strlen(JERR_INTERNAL), 0);
        }
        return c->keep_alive;
    }
    if (likely(rq->method == 0) && rq->target_len > 7 && memcmp(rq->target, "/posts/", 7) == 0) {
        const char *t = rq->target;
        size_t tl = rq->target_len;
        /* extra '/' or query string: unknown path -> 404 */
        for (size_t k = 7; k < tl; k++) {
            if (t[k] == '/' || t[k] == '?') {
                send_resp(c, rq, 404, "Not Found", JERR_NOT_FOUND, strlen(JERR_NOT_FOUND), 0);
                return c->keep_alive;
            }
        }
        unsigned long long idv = 0;
        if (unlikely(!valid_id(t + 7, tl - 7, &idv))) {
            send_resp(c, rq, 400, "Bad Request", JERR_INVALID_POST_ID, strlen(JERR_INVALID_POST_ID), 0);
            return c->keep_alive;
        }
        char ids[24];
        snprintf(ids, sizeof ids, "%llu", idv);
        int d = http_defer(c, rq, uptime_s, PG_ROUTE_POST, "q_post", 1,
                           ids, NULL, NULL, 0, 0);
        if (d == HTTP_DEFERRED) return HTTP_DEFERRED;
        const char *body = NULL;
        size_t bl = 0;
        char err[512];
        int r = pg_get_post(ids, &body, &bl, err, sizeof err);
        if (r == 0) {
            send_resp_ex(c, rq, 200, "OK", body, bl, 0, bl >= GZIP_MIN_POST);
        } else if (r == -2) {
            send_resp(c, rq, 404, "Not Found", JERR_POST_NOT_FOUND, strlen(JERR_POST_NOT_FOUND), 0);
        } else {
            send_resp(c, rq, 500, "Internal Server Error", JERR_INTERNAL, strlen(JERR_INTERNAL), 0);
        }
        return c->keep_alive;
    }
    if (unlikely(rq->method == 1) && target_eq(rq, "/posts")) {
        /* auth first */
        char sub[32], user[256];
        if (check_auth(c, rq, sub, sizeof sub, user, sizeof user) != 0)
            return c->keep_alive;
        /* missing Content-Length on POST -> malformed JSON */
        if (rq->content_length < 0) {
            send_resp(c, rq, 400, "Bad Request", JERR_MALFORMED_JSON, strlen(JERR_MALFORMED_JSON), 0);
            return c->keep_alive;
        }
        size_t declen = 0;
        int is_string = 0;
        int pr = extract_body(rq->body ? rq->body : "", rq->body_len,
                              t_dec, sizeof t_dec, &declen, &is_string);
        if (unlikely(pr < 0)) {
            send_resp(c, rq, 400, "Bad Request", JERR_MALFORMED_JSON, strlen(JERR_MALFORMED_JSON), 0);
            return c->keep_alive;
        }
        if (unlikely(pr == 0 || !is_string)) {
            send_resp(c, rq, 400, "Bad Request", JERR_BODY_REQUIRED, strlen(JERR_BODY_REQUIRED), 0);
            return c->keep_alive;
        }
        /* Greptile #3: reject embedded NULs before the DB. A body like
         * "a\u0000bbbb" decodes to bytes containing NUL; strlen-based copies
         * would truncate while body_len stays long (heap over-read in
         * render). Fail closed here so t_dec is NUL-free past declen. */
        if (unlikely(memchr(t_dec, '\0', declen) != NULL)) {
            send_resp(c, rq, 400, "Bad Request", JERR_MALFORMED_JSON, strlen(JERR_MALFORMED_JSON), 0);
            return c->keep_alive;
        }
        size_t tl2 = json_trim_inplace(t_dec, declen);
        t_dec[tl2] = '\0';
        if (tl2 == 0) {
            send_resp(c, rq, 400, "Bad Request", JERR_BODY_REQUIRED, strlen(JERR_BODY_REQUIRED), 0);
            return c->keep_alive;
        }
        if (json_charlen(t_dec, tl2) > 500) {
            send_resp(c, rq, 400, "Bad Request", JERR_BODY_TOO_LONG, strlen(JERR_BODY_TOO_LONG), 0);
            return c->keep_alive;
        }
        int d = http_defer(c, rq, uptime_s, PG_ROUTE_MKPOST, "q_mkpost", 2,
                           sub, t_dec, user, tl2, 0);
        if (d == HTTP_DEFERRED) {
            /* t_dec was copied into pend->p1; safe to reuse. */
            return HTTP_DEFERRED;
        }
        const char *body = NULL;
        size_t bl = 0;
        char err[512];
        if (pg_create_post(sub, t_dec, tl2, user, &body, &bl, err, sizeof err) == 0) {
            send_resp(c, rq, 201, "Created", body, bl, 0);
        } else {
            send_resp(c, rq, 500, "Internal Server Error", JERR_INTERNAL, strlen(JERR_INTERNAL), 0);
        }
        return c->keep_alive;
    }
    if (unlikely(rq->method == 1) && rq->target_len > 12 &&
        memcmp(rq->target, "/posts/", 7) == 0 &&
        memcmp(rq->target + rq->target_len - 5, "/like", 5) == 0) {
        /* auth before id (spec). */
        char sub[32], user[256];
        if (check_auth(c, rq, sub, sizeof sub, user, sizeof user) != 0)
            return c->keep_alive;
        size_t midlen = rq->target_len - 12;
        const char *mid = rq->target + 7;
        if (midlen == 0) {
            send_resp(c, rq, 400, "Bad Request", JERR_INVALID_POST_ID, strlen(JERR_INVALID_POST_ID), 0);
            return c->keep_alive;
        }
        int weird = 0;
        for (size_t k = 0; k < midlen; k++) {
            if (mid[k] == '/' || mid[k] == '?') { weird = 1; break; }
        }
        if (weird) {
            send_resp(c, rq, 404, "Not Found", JERR_NOT_FOUND, strlen(JERR_NOT_FOUND), 0);
            return c->keep_alive;
        }
        unsigned long long idv = 0;
        if (unlikely(!valid_id(mid, midlen, &idv))) {
            send_resp(c, rq, 400, "Bad Request", JERR_INVALID_POST_ID, strlen(JERR_INVALID_POST_ID), 0);
            return c->keep_alive;
        }
        char ids[24];
        snprintf(ids, sizeof ids, "%llu", idv);
        int d = http_defer(c, rq, uptime_s, PG_ROUTE_LIKE, "q_like", 2,
                           sub, ids, NULL, 0, idv);
        if (d == HTTP_DEFERRED) return HTTP_DEFERRED;
        int already = 0;
        char err[512];
        int r = pg_like(sub, ids, &already, err, sizeof err);
        if (r == 0) {
            char lb[96];
            int n = snprintf(lb, sizeof lb,
                "{\"liked\":true,\"already_liked\":%s,\"post_id\":%llu}",
                already ? "true" : "false", idv);
            if (n < 0 || (size_t)n >= sizeof lb) {
                send_resp(c, rq, 500, "Internal Server Error", JERR_INTERNAL, strlen(JERR_INTERNAL), 0);
            } else {
                send_resp(c, rq, already ? 200 : 201, already ? "OK" : "Created", lb, (size_t)n, 0);
            }
        } else if (r == -2) {
            send_resp(c, rq, 404, "Not Found", JERR_POST_NOT_FOUND, strlen(JERR_POST_NOT_FOUND), 0);
        } else {
            send_resp(c, rq, 500, "Internal Server Error", JERR_INTERNAL, strlen(JERR_INTERNAL), 0);
        }
        return c->keep_alive;
    }
    /* Unknown method on known path is out of scope -> 404. Unknown path -> 404. */
    send_resp(c, rq, 404, "Not Found", JERR_NOT_FOUND, strlen(JERR_NOT_FOUND), 0);
    return c->keep_alive;
}

/* Blocking fallback for a stashed request (send error, lost socket, or poll
 * transport error). Reuses the exact blocking pg_* path so wire behavior is
 * identical. Sends the response and tears down the stash. */
static int resume_blocking(struct conn_hot *c, http_req_t *tmp) {
    struct pg_pending *p = &c->cold->pend;
    int route = p->route;
    int ka = 1;
    if (route == PG_ROUTE_HEALTH) {
        char err[512]; err[0] = '\0';
        char body[1024];
        size_t bl;
        if (pg_health_check(err, sizeof err) == 0) {
            bl = json_render_health(body, sizeof body, p->uptime_s);
            send_resp(c, tmp, 200, "OK", body, bl, 0);
        } else {
            static const char degraded[] = "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"db unreachable\"}";
            if (!err[0]) snprintf(err, sizeof err, "db unreachable");
            fprintf(stderr, "health degraded: %s\n", err);
            send_resp(c, tmp, 503, "Service Unavailable", degraded, sizeof degraded - 1, 0);
        }
        ka = c->keep_alive;
    } else if (route == PG_ROUTE_FEED) {
        const char *body = NULL;
        size_t bl = 0;
        char err[512];
        if (pg_feed(&body, &bl, err, sizeof err) == 0) {
            send_resp_ex(c, tmp, 200, "OK", body, bl, 0, 1);
        } else {
            send_resp(c, tmp, 500, "Internal Server Error", JERR_INTERNAL, strlen(JERR_INTERNAL), 0);
        }
        ka = c->keep_alive;
    } else if (route == PG_ROUTE_POST) {
        const char *body = NULL;
        size_t bl = 0;
        char err[512];
        int r = pg_get_post(p->p0 ? p->p0 : "", &body, &bl, err, sizeof err);
        if (r == 0) {
            send_resp_ex(c, tmp, 200, "OK", body, bl, 0, bl >= GZIP_MIN_POST);
        } else if (r == -2) {
            log_post404("blocking", p->p0, NULL);
            send_resp(c, tmp, 404, "Not Found", JERR_POST_NOT_FOUND, strlen(JERR_POST_NOT_FOUND), 0);
        } else {
            send_resp(c, tmp, 500, "Internal Server Error", JERR_INTERNAL, strlen(JERR_INTERNAL), 0);
        }
        ka = c->keep_alive;
    } else if (route == PG_ROUTE_MKPOST) {
        const char *body = NULL;
        size_t bl = 0;
        char err[512];
        if (pg_create_post(p->p0 ? p->p0 : "", p->p1 ? p->p1 : "", p->body_len,
                           p->author ? p->author : "", &body, &bl, err, sizeof err) == 0) {
            send_resp(c, tmp, 201, "Created", body, bl, 0);
        } else {
            send_resp(c, tmp, 500, "Internal Server Error", JERR_INTERNAL, strlen(JERR_INTERNAL), 0);
        }
        ka = c->keep_alive;
    } else if (route == PG_ROUTE_LIKE) {
        int already = 0;
        char err[512];
        int r = pg_like(p->p0 ? p->p0 : "", p->p1 ? p->p1 : "", &already, err, sizeof err);
        if (r == 0) {
            char lb[96];
            int n = snprintf(lb, sizeof lb,
                "{\"liked\":true,\"already_liked\":%s,\"post_id\":%llu}",
                already ? "true" : "false", p->idv);
            if (n < 0 || (size_t)n >= sizeof lb) {
                send_resp(c, tmp, 500, "Internal Server Error", JERR_INTERNAL, strlen(JERR_INTERNAL), 0);
            } else {
                send_resp(c, tmp, already ? 200 : 201, already ? "OK" : "Created", lb, (size_t)n, 0);
            }
        } else if (r == -2) {
            send_resp(c, tmp, 404, "Not Found", JERR_POST_NOT_FOUND, strlen(JERR_POST_NOT_FOUND), 0);
        } else {
            send_resp(c, tmp, 500, "Internal Server Error", JERR_INTERNAL, strlen(JERR_INTERNAL), 0);
        }
        ka = c->keep_alive;
    } else {
        send_resp(c, tmp, 404, "Not Found", JERR_NOT_FOUND, strlen(JERR_NOT_FOUND), 0);
        ka = c->keep_alive;
    }
    http_pending_free(&c->cold->pend);
    c->state = CONN_ACTIVE;
    return ka;
}

int http_resume(struct conn_hot *c) {
    if (c->state != CONN_PG_WAIT) return c->keep_alive;
    struct pg_pending *p = &c->cold->pend;
    http_req_t tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.method = -1;
    tmp.ver_major = p->ver_major;
    tmp.ver_minor = p->ver_minor;
    tmp.connection = p->conn_hdr;
    tmp.connection_len = p->conn_hdr_len;
    tmp.accept_gzip = p->accept_gzip;
    /* Still queued behind a busy slot: retry the send. The ownership gate
     * (same owner+gen) guarantees the retry only fires once the slot is
     * genuinely free, never on a foreign in-flight query. */
    if (!p->sent) {
        const char *stmt = stmt_for_route(p->route);
        const char *par[2] = { p->p0, p->p1 };
        int rc = pg_send_async(p->slot, stmt, p->npar, p->npar ? par : NULL, c, p->pg_gen);
        int fd = pg_socket(p->slot);
        if (fd < 0 || rc == PG_ASYNC_ERR) {
            return resume_blocking(c, &tmp);
        }
        if (rc == PG_ASYNC_BUSY) return HTTP_DEFERRED;
        p->sent = 1;
    }
    PGresult *res = NULL;
    /* Owner-checked poll: a non-owner (stale/foreign waiter) never consumes
     * another waiter's completed result (Greptile #13). */
    int pr = pg_poll(p->slot, &res, c, p->pg_gen);
    if (pr == PG_POLL_PENDING) return HTTP_DEFERRED;
    if (pr == PG_POLL_ERROR) {
        if (res) PQclear(res);
        return resume_blocking(c, &tmp);
    }
    /* READY: render from the async result with identical wire semantics. */
    int ka = 1;
    if (p->route == PG_ROUTE_HEALTH) {
        char err[512]; err[0] = '\0';
        char body[1024];
        size_t bl;
        if (pg_render_health(res, err, sizeof err) == 0) {
            bl = json_render_health(body, sizeof body, p->uptime_s);
            send_resp(c, &tmp, 200, "OK", body, bl, 0);
        } else {
            static const char degraded[] = "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"db unreachable\"}";
            if (!err[0]) snprintf(err, sizeof err, "db unreachable");
            fprintf(stderr, "health degraded: %s\n", err);
            send_resp(c, &tmp, 503, "Service Unavailable", degraded, sizeof degraded - 1, 0);
        }
        ka = c->keep_alive;
    } else if (p->route == PG_ROUTE_FEED) {
        const char *body = NULL;
        size_t bl = 0;
        char err[512];
        if (pg_render_feed(res, &body, &bl, err, sizeof err) == 0) {
            send_resp_ex(c, &tmp, 200, "OK", body, bl, 0, 1);
        } else {
            send_resp(c, &tmp, 500, "Internal Server Error", JERR_INTERNAL, strlen(JERR_INTERNAL), 0);
        }
        ka = c->keep_alive;
    } else if (p->route == PG_ROUTE_POST) {
        const char *body = NULL;
        size_t bl = 0;
        char err[512];
        int r = pg_render_post(res, &body, &bl, err, sizeof err);
        if (r == 0) {
            send_resp_ex(c, &tmp, 200, "OK", body, bl, 0, bl >= GZIP_MIN_POST);
        } else if (r == -2) {
            log_post404("resume", p->p0, res);
            send_resp(c, &tmp, 404, "Not Found", JERR_POST_NOT_FOUND, strlen(JERR_POST_NOT_FOUND), 0);
        } else {
            send_resp(c, &tmp, 500, "Internal Server Error", JERR_INTERNAL, strlen(JERR_INTERNAL), 0);
        }
        ka = c->keep_alive;
    } else if (p->route == PG_ROUTE_MKPOST) {
        const char *body = NULL;
        size_t bl = 0;
        char err[512];
        if (pg_render_mkpost(res, p->p1 ? p->p1 : "", p->body_len,
                             p->author ? p->author : "", &body, &bl, err, sizeof err) == 0) {
            send_resp(c, &tmp, 201, "Created", body, bl, 0);
        } else {
            send_resp(c, &tmp, 500, "Internal Server Error", JERR_INTERNAL, strlen(JERR_INTERNAL), 0);
        }
        ka = c->keep_alive;
    } else if (p->route == PG_ROUTE_LIKE) {
        int already = 0;
        char err[512];
        int r = pg_render_like(res, &already, err, sizeof err);
        if (r == 0) {
            char lb[96];
            int n = snprintf(lb, sizeof lb,
                "{\"liked\":true,\"already_liked\":%s,\"post_id\":%llu}",
                already ? "true" : "false", p->idv);
            if (n < 0 || (size_t)n >= sizeof lb) {
                send_resp(c, &tmp, 500, "Internal Server Error", JERR_INTERNAL, strlen(JERR_INTERNAL), 0);
            } else {
                send_resp(c, &tmp, already ? 200 : 201, already ? "OK" : "Created", lb, (size_t)n, 0);
            }
        } else if (r == -2) {
            send_resp(c, &tmp, 404, "Not Found", JERR_POST_NOT_FOUND, strlen(JERR_POST_NOT_FOUND), 0);
        } else {
            send_resp(c, &tmp, 500, "Internal Server Error", JERR_INTERNAL, strlen(JERR_INTERNAL), 0);
        }
        ka = c->keep_alive;
    } else {
        send_resp(c, &tmp, 404, "Not Found", JERR_NOT_FOUND, strlen(JERR_NOT_FOUND), 0);
        ka = c->keep_alive;
    }
    if (res) PQclear(res);
    http_pending_free(&c->cold->pend);
    c->state = CONN_ACTIVE;
    return ka;
}
