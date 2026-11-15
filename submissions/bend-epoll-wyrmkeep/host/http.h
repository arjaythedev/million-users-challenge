// Requests for route A′'s host: the HTTP/1.1 lexer, the bearer-token check
// (A2) and the CreatePost body check (A3), each answering the codes
// host/wire.bend reads (route, id, auth, body: src/decode.bend). The
// behaviour follows the challenge's reference submission
// (vendor/million-users-challenge/submissions/typescript-bun-tbsvttr/
// server.ts:101-136, 170-183) and SPEC.md:64-87; the differences are listed
// in bench/servers/bend-epoll/README.md. Included by host/epoll.c only.
// Outside every proof.
#ifndef BM_HTTP_H
#define BM_HTTP_H

#include <math.h>
#include <strings.h>
#include <time.h>
// SHA256_Init/Update/Final are deprecated since OpenSSL 3.0 but present in 3
// and 4; their context can be copied, so the key's two blocks are absorbed
// once (http_key_set) and every check starts from a copy.
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include "json.h"

#define HTTP_HEAD 512u          // per-descriptor request buffer (host/epoll.c): a larger head grows it
#define HTTP_HEAD_MAX 65536u    // --max-head: largest value
#define HTTP_BODY_MAX 999999u   // --max-body: largest value, the largest Content-Length the lexer reads
#define HTTP_CLEN_SAT 1000000u  // a Content-Length value saturates here: over any --max-body, never wrapped

enum { AUTH_NO_BEARER, AUTH_BAD_TOKEN, AUTH_BAD_PAYLOAD, AUTH_USER };
enum { BODY_NONE_SENT, BODY_NOT_JSON, BODY_MISSING, BODY_TOO_LONG, BODY_TRIMMED };

typedef struct {
  uint32_t    route, id;
  const char* auth;     // the Authorization value without its outer blanks, or NULL
  size_t      auth_n;
  uint64_t    clen;
  bool        gzip;     // Accept-Encoding lists gzip, and not with a weight of zero
  uint32_t    head;     // bytes of the request line and headers, CRLF CRLF included
} HttpReq;

// ---- lexer ----

// The :id segment: digits only, 1 .. 4294967295; anything else is 0, the
// encoding's invalid id.
static uint32_t http_id(const char* p, size_t n) {
  if (n == 0 || n > 10) {
    return 0;
  }
  uint64_t v = 0;
  for (size_t i = 0; i < n; i += 1) {
    if (p[i] < '0' || p[i] > '9') {
      return 0;
    }
    v = v * 10 + (uint64_t)(p[i] - '0');
  }
  return v > 0xFFFFFFFFull ? 0 : (uint32_t)v;
}

static bool http_is(const char* p, size_t n, const char* lit) {
  return n == strlen(lit) && memcmp(p, lit, n) == 0;
}

// route and id from the method and the path; a query string makes the path
// unknown
static void http_route(const char* m, size_t mn, const char* p, size_t pn, uint32_t* route, uint32_t* id) {
  bool get = http_is(m, mn, "GET"), post = http_is(m, mn, "POST");
  *route = 5;
  *id    = 0;
  if (get && http_is(p, pn, "/health")) {
    *route = 0;
  } else if (get && http_is(p, pn, "/feed")) {
    *route = 1;
  } else if (post && http_is(p, pn, "/posts")) {
    *route = 3;
  } else if (pn > 7 && memcmp(p, "/posts/", 7) == 0) {
    const char* s  = p + 7;
    size_t      sn = pn - 7;
    const char* sl = memchr(s, '/', sn);
    if (get && sl == NULL) {
      *route = 2;
      *id    = http_id(s, sn);
    } else if (post && sl != NULL && http_is(sl, (size_t)(s + sn - sl), "/like")) {
      *route = 4;
      *id    = http_id(s, (size_t)(sl - s));
    }
  }
}

static void http_blank_trim(const char** v, const char** e) {
  while (*v < *e && (**v == ' ' || **v == '\t')) {
    *v += 1;
  }
  while (*e > *v && ((*e)[-1] == ' ' || (*e)[-1] == '\t')) {
    *e -= 1;
  }
}

// Content-Length's value between optional blanks: decimal digits only, of any count. The
// value saturates at HTTP_CLEN_SAT, which no --max-body reaches, so a 20-digit length is a
// body over the cap (413) and never wraps to a small one.
static bool http_clen(const char* v, const char* e, uint64_t* clen) {
  http_blank_trim(&v, &e);
  size_t n = (size_t)(e - v);
  if (n == 0) {
    return false;
  }
  uint64_t x = 0;
  for (size_t i = 0; i < n; i += 1) {
    if (v[i] < '0' || v[i] > '9') {
      return false;
    }
    x = x * 10 + (uint64_t)(v[i] - '0');
    if (x > HTTP_CLEN_SAT) {
      x = HTTP_CLEN_SAT;
    }
  }
  *clen = x;
  return true;
}

// ---- line ends ----

// p[0 ..< n]'s first "\r\n", or NULL: memchr finds each '\r', the byte after it decides. As
// memmem(p, n, "\r\n", 2) answers; glibc's memmem cost 486 user-space cycles a request for the
// four to seven searches of a head (issue #67, .local/checks/opt/67/p1-ibs-cycles).
static const char* http_crlf(const char* p, size_t n) {
  const char* end = p + n;
  while (end - p >= 2) {
    const char* c = memchr(p, '\r', (size_t)(end - p) - 1);
    if (c == NULL) {
      return NULL;
    }
    if (c[1] == '\n') {
      return c;
    }
    p = c + 1;
  }
  return NULL;
}

// ---- header names ----

// h[0 ..< hn] begins with name's n bytes, letters in either case: as
// hn >= n && strncasecmp(h, name, n) == 0 answers. name is in lower case and begins with a
// letter, so a first byte that is not that letter in either case decides without the call
// (a request of mload's has three or four header lines and at most two of the names).
static inline bool http_name(const char* h, size_t hn, const char* name, size_t n) {
  return hn >= n && (h[0] | 0x20) == name[0] && strncasecmp(h, name, n) == 0;
}

// p[0 ..< n], the value of an Accept-Encoding, asks for gzip: it holds "gzip" (where
// memmem(p, n, "gzip", 4) finds it), and not with a weight of zero. The weight is the "q="
// after the name's ';', blanks allowed on both sides of the ';' and the q in either case
// (RFC 9110, 12.4.2). It is zero as "0", "0." and "0." with zeros only after it, when the
// element ends there (',', ';', a blank or the value's end). Anything else after the name
// leaves the answer as the name alone gives it.
static inline bool http_gzip(const char* p, size_t n) {
  const char* e = p + n;
  for (const char* g = p; e - g >= 4; g += 1) {
    if (memcmp(g, "gzip", 4) != 0) {
      continue;
    }
    const char* w = g + 4;
    while (w < e && (*w == ' ' || *w == '\t')) {
      w += 1;
    }
    if (w == e || *w != ';') {
      return true;
    }
    w += 1;
    while (w < e && (*w == ' ' || *w == '\t')) {
      w += 1;
    }
    if (e - w < 3 || (w[0] | 0x20) != 'q' || w[1] != '=' || w[2] != '0') {
      return true;
    }
    w += 3;
    if (w < e && *w == '.') {
      w += 1;
      while (w < e && *w == '0') {
        w += 1;
      }
    }
    return w < e && *w != ',' && *w != ';' && *w != ' ' && *w != '\t';
  }
  return false;
}

// ---- the request's head ----

// 1: one complete request in b[0 .. r->head + r->clen); 0: incomplete, and *need is the
// room the next read has to have when the buffer (cap bytes) is full, else 0; below 0:
// the host answers the request itself, with the status that is the negative of the
// result, then closes the connection: -400 (a bad or a second Content-Length), -411 (a
// Transfer-Encoding header: no transfer coding is decoded), -413 (a Content-Length over
// max_body), -431 (no end of head within max_head bytes; a head of exactly max_head is
// fine). A head that does not fit cap asks for twice the room, at most max_head.
//
// One pass over the lines, each "\r\n" found once. The head ends at the first "\r\n" that
// another follows, which is where the first "\r\n\r\n" starts (no "\r\n" starts on the '\n'
// of another), and nothing is known to be a request before that: the headers are read into q
// as the lines pass, the first one that refuses the request stops the reading (bad) and not the
// search, and r is written only once the end is found.
static int http_lex(const char* b, uint32_t used, uint32_t cap, uint32_t max_head, uint32_t max_body,
  HttpReq* r, uint32_t* need) {
  *need = 0;
  uint32_t    look = used < max_head ? used : max_head;
  const char* lim  = b + look;
  const char* eol  = http_crlf(b, look);
  const char* end  = NULL;
  HttpReq     q    = { .route = 5 };
  bool        seen = false;
  int         bad  = 0;
  for (const char* c = eol; c != NULL;) {
    const char* h  = c + 2;
    const char* he = http_crlf(h, (size_t)(lim - h));
    if (he == h) {
      end = c;
      break;
    }
    if (he != NULL && bad == 0) {
      size_t hn = (size_t)(he - h);
      switch (h[0] | 0x20) {
      case 'a':
        if (http_name(h, hn, "authorization:", 14)) {
          if (q.auth == NULL) {
            const char* v = h + 14;
            const char* e = he;
            http_blank_trim(&v, &e);
            q.auth   = v;
            q.auth_n = (size_t)(e - v);
          }
        } else if (http_name(h, hn, "accept-encoding:", 16)) {
          q.gzip = http_gzip(h + 16, hn - 16);
        }
        break;
      case 'c':
        if (http_name(h, hn, "content-length:", 15)) {
          if (seen || !http_clen(h + 15, he, &q.clen)) {
            bad = -400;
          }
          seen = true;
        }
        break;
      case 't':
        if (http_name(h, hn, "transfer-encoding:", 18)) {
          bad = -411;
        }
        break;
      }
    }
    c = he;
  }
  if (end == NULL) {
    if (used >= max_head) {
      return -431;
    }
    if (used >= cap) {
      *need = cap > max_head / 2 ? max_head : cap * 2;
    }
    return 0;
  }
  uint32_t    head = (uint32_t)(end - b) + 4;
  const char* sp1  = memchr(b, ' ', (size_t)(eol - b));
  const char* sp2  = sp1 ? memchr(sp1 + 1, ' ', (size_t)(eol - sp1 - 1)) : NULL;
  q.head = head;
  if (sp1 != NULL && sp2 != NULL) {
    http_route(b, (size_t)(sp1 - b), sp1 + 1, (size_t)(sp2 - sp1 - 1), &q.route, &q.id);
  }
  *r = q;
  if (bad != 0) {
    return bad;
  }
  if (r->clen > max_body) {
    return -413;
  }
  uint32_t total = head + (uint32_t)r->clen;
  if (used < total) {
    *need = total > cap ? total : 0;
    return 0;
  }
  return 1;
}

// ---- A2: the bearer token ----

typedef struct {
  SHA256_CTX inner, outer;  // the key's ipad and opad blocks absorbed
} HttpKey;

static HttpKey http_key;

static void http_key_set(const char* secret) {
  unsigned char k[SHA256_CBLOCK] = { 0 };
  size_t        n = strlen(secret);
  if (n > SHA256_CBLOCK) {
    SHA256((const unsigned char*)secret, n, k);
  } else {
    memcpy(k, secret, n);
  }
  unsigned char ip[SHA256_CBLOCK], op[SHA256_CBLOCK];
  for (int i = 0; i < SHA256_CBLOCK; i += 1) {
    ip[i] = k[i] ^ 0x36;
    op[i] = k[i] ^ 0x5c;
  }
  SHA256_Init(&http_key.inner);
  SHA256_Update(&http_key.inner, ip, sizeof ip);
  SHA256_Init(&http_key.outer);
  SHA256_Update(&http_key.outer, op, sizeof op);
  OPENSSL_cleanse(k, sizeof k);
  OPENSSL_cleanse(ip, sizeof ip);
  OPENSSL_cleanse(op, sizeof op);
}

static void http_hmac(const char* msg, size_t n, unsigned char mac[32]) {
  SHA256_CTX    c = http_key.inner;
  unsigned char in[32];
  SHA256_Update(&c, msg, n);
  SHA256_Final(in, &c);
  c = http_key.outer;
  SHA256_Update(&c, in, sizeof in);
  SHA256_Final(mac, &c);
}

static const char http_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int http_b64v(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}

// base64url without padding (or with one or two '=' at the end) into out;
// false on any other character or a length that no encoding has
static bool http_b64_decode(const char* p, size_t n, JBuf* out) {
  while (n > 0 && p[n - 1] == '=') {
    n -= 1;
  }
  if (n % 4 == 1) {
    return false;
  }
  out->n = 0;
  jb_need(out, n * 3 / 4 + 3);
  uint32_t acc = 0, bits = 0;
  for (size_t i = 0; i < n; i += 1) {
    int v = http_b64v(p[i]);
    if (v < 0) {
      return false;
    }
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->p[out->n++] = (char)((acc >> bits) & 0xFF);
    }
  }
  return true;
}

static void http_b64_encode(const unsigned char* p, size_t n, char* out) {
  size_t o = 0;
  uint32_t acc = 0, bits = 0;
  for (size_t i = 0; i < n; i += 1) {
    acc = (acc << 8) | p[i];
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out[o++] = http_b64[(acc >> bits) & 63];
    }
  }
  if (bits > 0) {
    out[o++] = http_b64[(acc << (6 - bits)) & 63];
  }
}

typedef struct {
  JBuf raw, utf8;
  JDoc doc;
} HttpScratch;

static HttpScratch http_hdr, http_pay, http_bod;

// One JWT part: base64url, then UTF-8 repaired, then JSON. The root node, or -1.
static int32_t http_part(HttpScratch* s, const char* p, size_t n) {
  if (!http_b64_decode(p, n, &s->raw)) {
    return -1;
  }
  utf8_repair(s->raw.p, s->raw.n, &s->utf8);
  return json_parse(&s->doc, s->utf8.p, s->utf8.n);
}

static double http_now_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

// The auth code of r's Authorization value; for AUTH_USER, *uid and the
// username in name (well-formed UTF-8). The reference's order: parts, both
// JSON parts parse, alg is "HS256", the signature, the payload is an object,
// exp, nbf; then sub and username.
static uint32_t http_auth(const HttpReq* r, uint32_t* uid, JBuf* name) {
  *uid    = 0;
  name->n = 0;
  if (r->auth == NULL || r->auth_n < 7 || memcmp(r->auth, "Bearer ", 7) != 0) {
    return AUTH_NO_BEARER;
  }
  const char* t  = r->auth + 7;
  size_t      n  = r->auth_n - 7;
  const char* d1 = memchr(t, '.', n);
  if (d1 == NULL || d1 == t) {
    return AUTH_BAD_TOKEN;
  }
  const char* d2 = memchr(d1 + 1, '.', (size_t)(t + n - d1 - 1));
  if (d2 == NULL || memchr(d2 + 1, '.', (size_t)(t + n - d2 - 1)) != NULL) {
    return AUTH_BAD_TOKEN;
  }
  int32_t h = http_part(&http_hdr, t, (size_t)(d1 - t));
  int32_t p = http_part(&http_pay, d1 + 1, (size_t)(d2 - d1 - 1));
  if (h < 0 || p < 0) {
    return AUTH_BAD_TOKEN;
  }
  int32_t alg = json_get(&http_hdr.doc, h, "alg");
  if (alg < 0 || http_hdr.doc.v[alg].kind != J_STR || http_hdr.doc.v[alg].n != 5
    || memcmp(json_text(&http_hdr.doc, alg), "HS256", 5) != 0) {
    return AUTH_BAD_TOKEN;
  }
  unsigned char mac[32];
  char          want[43];
  http_hmac(t, (size_t)(d2 - t), mac);
  http_b64_encode(mac, sizeof mac, want);
  const char* sig  = d2 + 1;
  size_t      sign = (size_t)(t + n - sig);
  if (sign != sizeof want || CRYPTO_memcmp(sig, want, sizeof want) != 0) {
    return AUTH_BAD_TOKEN;
  }
  JDoc* d = &http_pay.doc;
  if (d->v[p].kind != J_OBJ && d->v[p].kind != J_ARR) {
    return AUTH_BAD_TOKEN;
  }
  double  now = http_now_s();
  int32_t exp = json_get(d, p, "exp");
  if (exp < 0 || d->v[exp].kind != J_NUM || !(d->v[exp].num > now)) {
    return AUTH_BAD_TOKEN;
  }
  int32_t nbf = json_get(d, p, "nbf");
  if (nbf >= 0 && (d->v[nbf].kind != J_NUM || d->v[nbf].num > now)) {
    return AUTH_BAD_TOKEN;
  }
  int32_t sub = json_get(d, p, "sub");
  int32_t usr = json_get(d, p, "username");
  if (sub < 0 || d->v[sub].kind != J_STR || usr < 0 || d->v[usr].kind != J_STR) {
    return AUTH_BAD_PAYLOAD;
  }
  const char* s  = json_text(d, sub);
  size_t      sn = d->v[sub].n;
  uint64_t    v  = 0;
  if (sn == 0) {
    return AUTH_BAD_PAYLOAD;
  }
  for (size_t i = 0; i < sn; i += 1) {
    if (s[i] < '0' || s[i] > '9') {
      return AUTH_BAD_PAYLOAD;
    }
    v = v * 10 + (uint64_t)(s[i] - '0');
    if (v > 0xFFFFFFFFull) {
      return AUTH_BAD_PAYLOAD;  // past U32; the reference takes up to 2^53 - 1
    }
  }
  if (v == 0) {
    return AUTH_BAD_PAYLOAD;
  }
  *uid = (uint32_t)v;
  jb_put(name, json_text(d, usr), d->v[usr].n);
  return AUTH_USER;
}

// ---- A3: the CreatePost body ----

// The body code of b[0 ..< n]; for BODY_TRIMMED the trimmed text in text.
static uint32_t http_body(const char* b, size_t n, JBuf* text) {
  text->n = 0;
  if (n == 0) {
    return BODY_NONE_SENT;
  }
  utf8_repair(b, n, &http_bod.utf8);
  JDoc*   d = &http_bod.doc;
  int32_t r = json_parse(d, http_bod.utf8.p, http_bod.utf8.n);
  if (r < 0) {
    return BODY_NOT_JSON;
  }
  int32_t v = json_get(d, r, "body");
  if (v < 0 || d->v[v].kind != J_STR) {
    return BODY_MISSING;
  }
  size_t at, len;
  size_t points = js_trim(json_text(d, v), d->v[v].n, &at, &len);
  if (points == 0) {
    return BODY_MISSING;
  }
  if (points > 500) {
    return BODY_TOO_LONG;
  }
  if (memchr(json_text(d, v) + at, 0, len) != NULL) {
    return BODY_NOT_JSON;  // PostgreSQL text holds no U+0000; the reference answers 500
  }
  jb_put(text, json_text(d, v) + at, len);
  return BODY_TRIMMED;
}

#endif
