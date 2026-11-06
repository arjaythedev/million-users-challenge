// Feed API on Postgres: one epoll loop per CPU, each with its own SO_REUSEPORT listeners on PORT and
// EXTRA_PORT and its own libpq connections in pipeline mode. See README.md.
// Config comes only from DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include "igzip_lib.h"
#include "libpq-fe.h"

#define RBUF (64 * 1024)
#define MAXREQ (64 * 1024)  // header + body cap per request
#define OBUF (256 * 1024)   // one response; the largest (20 posts of 500 escaped chars) is ~70 KB
#define GZMIN 1024          // smaller bodies (everything but the feed) go out uncompressed
#define BATCH_MIN 32        // an iteration with fewer events than this ...
#define BATCH_NS 2000000    // ... is followed by a 2 ms pause (see README.md, "Batching window")

// One per descriptor, so a million idle keep-alive connections cost 40 MB here. Buffers exist only
// while a request or response is split across reads or writes.
typedef struct {
  char *in; uint32_t inlen, incap;     // unparsed request bytes carried between reads
  char *out; uint32_t outlen, outoff;  // unsent response bytes after a short write
  uint32_t gen;                        // bumped on close, so a late query result is dropped
  uint8_t open, busy, closing;         // busy: waiting for its query; requests after it wait too
} conn_t;
static conn_t *conns; static long maxfd;

static __thread char *arena; static __thread size_t alen;
static __thread time_t now_s;
static time_t started;

static inline void put(const void *p, size_t n) { memcpy(arena + alen, p, n); alen += n; }
#define PUTS(s) put(s, sizeof(s) - 1)
static inline void putu(uint64_t v) {
  char b[24]; int i = 24;
  do { b[--i] = '0' + v % 10; v /= 10; } while (v);
  put(b + i, 24 - i);
}
static const char hexd[] = "0123456789abcdef";
static void putjs(const unsigned char *s, size_t n) {  // JSON string contents, escaped
  size_t i = 0, run = 0;
  for (; i < n; i++) {
    unsigned char c = s[i];
    if (c >= 0x20 && c != '"' && c != '\\') continue;
    put(s + run, i - run); run = i + 1;
    char e[6] = {'\\', 0};
    switch (c) {
      case '"': e[1] = '"'; put(e, 2); break;
      case '\\': e[1] = '\\'; put(e, 2); break;
      case '\n': e[1] = 'n'; put(e, 2); break;
      case '\r': e[1] = 'r'; put(e, 2); break;
      case '\t': e[1] = 't'; put(e, 2); break;
      default: e[1] = 'u'; e[2] = '0'; e[3] = '0'; e[4] = hexd[c >> 4]; e[5] = hexd[c & 15]; put(e, 6);
    }
  }
  put(s + run, n - run);
}
static inline void put2(unsigned v) { char b[2] = {'0' + v / 10, '0' + v % 10}; put(b, 2); }
// timestamptz in binary: microseconds since 2000-01-01 UTC -> 2025-12-31T23:59:19.409Z (truncated to ms)
static void put_ts(int64_t us) {
  int64_t s = us / 1000000, r = us % 1000000;
  if (r < 0) { r += 1000000; s--; }
  s += 946684800;
  int64_t days = s / 86400, sod = s % 86400;
  if (sod < 0) { sod += 86400; days--; }
  days += 719468;  // civil_from_days (H. Hinnant)
  int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  unsigned doe = days - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
  int64_t y = yoe + era * 400 + (m <= 2);
  put2(y / 100); put2(y % 100); PUTS("-"); put2(m); PUTS("-"); put2(d); PUTS("T");
  put2(sod / 3600); PUTS(":"); put2(sod / 60 % 60); PUTS(":"); put2(sod % 60);
  unsigned ms = r / 1000; char b[5] = {'.', '0' + ms / 100, '0' + ms / 10 % 10, '0' + ms % 10, 'Z'}; put(b, 5);
}

// response head with a blank Content-Length that finish() fills in
static __thread size_t body_start, cl_pos;
static __thread int want_gz; static __thread char *zbuf; static __thread size_t zlen;
// ISA-L igzip at level 0 (its fastest: one hash lookup per position, no lazy matching), stateless.
static size_t gzip(const char *in, size_t n, char *out, size_t cap) {
  struct isal_zstream s; isal_deflate_stateless_init(&s);
  s.next_in = (uint8_t *)in; s.avail_in = n; s.next_out = (uint8_t *)out; s.avail_out = cap; s.gzip_flag = IGZIP_GZIP; s.level = 0;
  return isal_deflate_stateless(&s) == COMP_OK ? cap - s.avail_out : 0;
}
static void begin_resp(int status, int close) {
  alen = 0; zlen = 0;
  switch (status) {
    case 200: PUTS("HTTP/1.1 200 OK\r\n"); break;
    case 201: PUTS("HTTP/1.1 201 Created\r\n"); break;
    case 400: PUTS("HTTP/1.1 400 Bad Request\r\n"); break;
    case 401: PUTS("HTTP/1.1 401 Unauthorized\r\n"); break;
    case 404: PUTS("HTTP/1.1 404 Not Found\r\n"); break;
    case 503: PUTS("HTTP/1.1 503 Service Unavailable\r\n"); break;
    default: PUTS("HTTP/1.1 500 Internal Server Error\r\n"); break;
  }
  if (close) PUTS("Connection: close\r\n");
  PUTS("Content-Type: application/json\r\nContent-Length:       \r\n\r\n");
  cl_pos = alen - 10; body_start = alen;
}
// Fills in Content-Length; with Accept-Encoding: gzip, a large body is compressed into zbuf instead.
static void finish(void) {
  size_t n = alen - body_start; char *p = arena + cl_pos + 6;
  if (want_gz && n >= GZMIN && (zlen = gzip(arena + body_start, n, zbuf, OBUF))) {
    alen = cl_pos - 16;  // rewrite from the Content-Length line on
    PUTS("Content-Encoding: gzip\r\nContent-Length: "); putu(zlen); PUTS("\r\n\r\n");
    return;
  }
  do { *--p = '0' + n % 10; n /= 10; } while (n);
}
static void err(int status, const char *msg, int close) {
  begin_resp(status, close); PUTS("{\"error\":\""); put(msg, strlen(msg)); PUTS("\"}"); finish();
}

// ---------- SHA-256 / HMAC ----------
typedef struct { uint32_t h[8]; uint64_t n; unsigned char b[64]; uint32_t bl; } sha_t;
static const uint32_t K[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
  0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
  0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
  0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(uint32_t *h, const unsigned char *p) {
  uint32_t w[64], a, b, c, d, e, f, g, k, t1, t2;
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | p[4*i+1] << 16 | p[4*i+2] << 8 | p[4*i+3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }
  a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; k = h[7];
  for (int i = 0; i < 64; i++) {
    t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
    t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
static void sha_init(sha_t *s) {
  static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  memcpy(s->h, iv, 32); s->n = 0; s->bl = 0;
}
static void sha_upd(sha_t *s, const unsigned char *p, size_t n) {
  s->n += n;
  if (s->bl) { size_t t = 64 - s->bl; if (t > n) t = n; memcpy(s->b + s->bl, p, t); s->bl += t; p += t; n -= t; if (s->bl == 64) { sha_block(s->h, s->b); s->bl = 0; } }
  for (; n >= 64; p += 64, n -= 64) sha_block(s->h, p);
  if (n) { memcpy(s->b, p, n); s->bl = n; }
}
static void sha_fin(sha_t *s, unsigned char *out) {
  uint64_t bits = s->n * 8; unsigned char pad = 0x80, z[64] = {0}, len[8];
  sha_upd(s, &pad, 1);
  sha_upd(s, z, (s->bl <= 56 ? 56 : 120) - s->bl);
  for (int i = 0; i < 8; i++) len[i] = bits >> (56 - 8 * i);
  sha_upd(s, len, 8);
  for (int i = 0; i < 8; i++) { out[4*i] = s->h[i] >> 24; out[4*i+1] = s->h[i] >> 16; out[4*i+2] = s->h[i] >> 8; out[4*i+3] = s->h[i]; }
}
static sha_t hm_in, hm_out;  // HMAC key schedule: hash state after absorbing key^ipad / key^opad
static void hmac_key(const char *key) {
  unsigned char k[64] = {0}, pad[64]; size_t kl = strlen(key);
  if (kl > 64) { sha_t s; sha_init(&s); sha_upd(&s, (const unsigned char *)key, kl); sha_fin(&s, k); } else memcpy(k, key, kl);
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
  sha_init(&hm_in); sha_upd(&hm_in, pad, 64);
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
  sha_init(&hm_out); sha_upd(&hm_out, pad, 64);
}
static void hmac(const unsigned char *m, size_t n, unsigned char *out) {
  sha_t s = hm_in; unsigned char ih[32];
  sha_upd(&s, m, n); sha_fin(&s, ih);
  s = hm_out; sha_upd(&s, ih, 32); sha_fin(&s, out);
}

// ---------- base64url ----------
static signed char b64t[256];
static int b64dec(const char *s, size_t n, unsigned char *out) {  // -1 on bad input
  if (n % 4 == 1) return -1;
  uint32_t acc = 0; int bits = 0, o = 0;
  for (size_t i = 0; i < n; i++) {
    int v = b64t[(unsigned char)s[i]]; if (v < 0) return -1;
    acc = acc << 6 | v; bits += 6;
    if (bits >= 8) { bits -= 8; out[o++] = acc >> bits; }
  }
  return o;
}

// ---------- JSON (strict, RFC 8259) ----------
typedef struct { const char *p, *e; } js_t;
static inline void ws(js_t *j) { while (j->p < j->e && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++; }
static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int utf8_len(const unsigned char *p, const unsigned char *e) {  // length of one valid UTF-8 sequence, 0 if invalid
  unsigned c = p[0];
  if (c < 0x80) return 1;
  if (c >= 0xc2 && c <= 0xdf) return e - p >= 2 && (p[1] & 0xc0) == 0x80 ? 2 : 0;
  if (c >= 0xe0 && c <= 0xef) {
    if (e - p < 3 || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80) return 0;
    if ((c == 0xe0 && p[1] < 0xa0) || (c == 0xed && p[1] > 0x9f)) return 0;
    return 3;
  }
  if (c >= 0xf0 && c <= 0xf4) {
    if (e - p < 4 || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80 || (p[3] & 0xc0) != 0x80) return 0;
    if ((c == 0xf0 && p[1] < 0x90) || (c == 0xf4 && p[1] > 0x8f)) return 0;
    return 4;
  }
  return 0;
}
// Parses the string at j->p (on its opening quote), decoding UTF-8 into out when out is non-NULL.
// Returns the decoded length, -1 on a syntax error, -2 if it doesn't fit in cap (the string is still validated).
static long jstr(js_t *j, char *out, size_t cap) {
  const unsigned char *p = (const unsigned char *)j->p + 1, *e = (const unsigned char *)j->e; size_t o = 0; int over = 0;
  while (p < e) {
    unsigned c = *p;
    if (c == '"') { j->p = (const char *)p + 1; return over ? -2 : (long)o; }
    if (c < 0x20) return -1;
    if (c == '\\') {
      if (++p >= e) return -1;
      char ch;
      switch (*p) {
        case '"': ch = '"'; break; case '\\': ch = '\\'; break; case '/': ch = '/'; break;
        case 'b': ch = '\b'; break; case 'f': ch = '\f'; break; case 'n': ch = '\n'; break;
        case 'r': ch = '\r'; break; case 't': ch = '\t'; break;
        case 'u': {
          if (e - p < 5) return -1;
          int a = hexv(p[1]), b = hexv(p[2]), c2 = hexv(p[3]), d = hexv(p[4]);
          if ((a | b | c2 | d) < 0) return -1;
          uint32_t cp = a << 12 | b << 8 | c2 << 4 | d; p += 4;
          if (cp >= 0xd800 && cp <= 0xdbff && e - p >= 7 && p[1] == '\\' && p[2] == 'u') {
            int a2 = hexv(p[3]), b2 = hexv(p[4]), c3 = hexv(p[5]), d2 = hexv(p[6]);
            if ((a2 | b2 | c3 | d2) < 0) return -1;
            uint32_t lo = a2 << 12 | b2 << 8 | c3 << 4 | d2;
            if (lo >= 0xdc00 && lo <= 0xdfff) { cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00); p += 6; }
          }
          if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;  // lone surrogate: U+FFFD, as JS TextEncoder does
          char u[4]; int ul;
          if (cp < 0x80) { u[0] = cp; ul = 1; }
          else if (cp < 0x800) { u[0] = 0xc0 | cp >> 6; u[1] = 0x80 | (cp & 63); ul = 2; }
          else if (cp < 0x10000) { u[0] = 0xe0 | cp >> 12; u[1] = 0x80 | (cp >> 6 & 63); u[2] = 0x80 | (cp & 63); ul = 3; }
          else { u[0] = 0xf0 | cp >> 18; u[1] = 0x80 | (cp >> 12 & 63); u[2] = 0x80 | (cp >> 6 & 63); u[3] = 0x80 | (cp & 63); ul = 4; }
          if (out && o + ul > cap) { out = NULL; over = 1; }
          if (out) memcpy(out + o, u, ul);
          o += ul; p++; continue;
        }
        default: return -1;
      }
      if (out && o >= cap) { out = NULL; over = 1; }
      if (out) out[o] = ch;
      o++; p++; continue;
    }
    int l = utf8_len(p, e); if (!l) return -1;
    if (out && o + l > cap) { out = NULL; over = 1; }
    if (out) memcpy(out + o, p, l);
    o += l; p += l;
  }
  return -1;
}
static int jnumber(js_t *j) {
  const char *p = j->p, *e = j->e;
  if (p < e && *p == '-') p++;
  if (p >= e) return 0;
  if (*p == '0') p++; else if (*p >= '1' && *p <= '9') while (p < e && *p >= '0' && *p <= '9') p++; else return 0;
  if (p < e && *p == '.') { p++; if (p >= e || *p < '0' || *p > '9') return 0; while (p < e && *p >= '0' && *p <= '9') p++; }
  if (p < e && (*p == 'e' || *p == 'E')) { p++; if (p < e && (*p == '+' || *p == '-')) p++; if (p >= e || *p < '0' || *p > '9') return 0; while (p < e && *p >= '0' && *p <= '9') p++; }
  j->p = p; return 1;
}
static int jlit(js_t *j, const char *s, size_t n) { if ((size_t)(j->e - j->p) < n || memcmp(j->p, s, n)) return 0; j->p += n; return 1; }
static int jvalue(js_t *j, int depth) {  // validates and skips one value
  if (depth > 64) return 0;
  ws(j); if (j->p >= j->e) return 0;
  switch (*j->p) {
    case '"': return jstr(j, NULL, 0) >= 0;
    case '{':
      j->p++; ws(j);
      if (j->p < j->e && *j->p == '}') { j->p++; return 1; }
      for (;;) {
        ws(j); if (j->p >= j->e || *j->p != '"' || jstr(j, NULL, 0) < 0) return 0;
        ws(j); if (j->p >= j->e || *j->p++ != ':') return 0;
        if (!jvalue(j, depth + 1)) return 0;
        ws(j); if (j->p >= j->e) return 0;
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == '}') { j->p++; return 1; }
        return 0;
      }
    case '[':
      j->p++; ws(j);
      if (j->p < j->e && *j->p == ']') { j->p++; return 1; }
      for (;;) {
        if (!jvalue(j, depth + 1)) return 0;
        ws(j); if (j->p >= j->e) return 0;
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == ']') { j->p++; return 1; }
        return 0;
      }
    case 't': return jlit(j, "true", 4);
    case 'f': return jlit(j, "false", 5);
    case 'n': return jlit(j, "null", 4);
    default: return jnumber(j);
  }
}
// Validates a whole document. For a top-level object it records the raw span of the last value of
// each wanted key (the last duplicate wins, as in JSON.parse). Returns 0 if the document is invalid.
typedef struct { const char *key; const char *v, *ve; } want_t;
static int jdoc(const char *s, size_t n, want_t *w, int nw) {
  js_t j = {s, s + n};
  for (int i = 0; i < nw; i++) w[i].v = NULL;
  ws(&j);
  if (j.p < j.e && *j.p == '{') {
    j.p++; ws(&j);
    if (j.p < j.e && *j.p == '}') j.p++;
    else for (;;) {
      char kb[32]; ws(&j);
      if (j.p >= j.e || *j.p != '"') return 0;
      long kl = jstr(&j, kb, sizeof kb);
      if (kl == -1) return 0;
      ws(&j); if (j.p >= j.e || *j.p++ != ':') return 0;
      ws(&j); const char *vs = j.p;
      if (!jvalue(&j, 1)) return 0;
      if (kl >= 0) for (int i = 0; i < nw; i++) if ((size_t)kl == strlen(w[i].key) && !memcmp(kb, w[i].key, kl)) { w[i].v = vs; w[i].ve = j.p; }
      ws(&j); if (j.p >= j.e) return 0;
      if (*j.p == ',') { j.p++; continue; }
      if (*j.p == '}') { j.p++; break; }
      return 0;
    }
  } else if (!jvalue(&j, 0)) return 0;
  ws(&j);
  return j.p == j.e;
}

// ---------- auth ----------
// Returns NULL and fills uid/uname on success, else the message to send with the 401.
static const char *auth(const char *h, size_t hl, int64_t *uid, char *uname, size_t *unl) {
  static const char *bad = "invalid or expired token", *payload = "invalid token payload";
  if (!h || hl < 7 || memcmp(h, "Bearer ", 7)) return "missing bearer token";
  const char *t = h + 7, *te = h + hl, *d1 = memchr(t, '.', te - t), *d2;
  if (!d1) return bad;
  d2 = memchr(d1 + 1, '.', te - d1 - 1);
  if (!d2 || memchr(d2 + 1, '.', te - d2 - 1)) return bad;
  unsigned char sig[48], mac[32], hdr[512], pl[2048];
  if (te - d2 - 1 != 43 || b64dec(d2 + 1, 43, sig) != 32) return bad;
  hmac((const unsigned char *)t, d2 - t, mac);
  unsigned diff = 0; for (int i = 0; i < 32; i++) diff |= sig[i] ^ mac[i];
  if (diff) return bad;
  if (d1 - t > 680 || d2 - d1 - 1 > 2700) return bad;
  int hn = b64dec(t, d1 - t, hdr), pn = b64dec(d1 + 1, d2 - d1 - 1, pl);
  if (hn < 0 || pn < 0) return bad;
  want_t hw[1] = {{"alg"}};
  if (!jdoc((char *)hdr, hn, hw, 1) || !hw[0].v || *hw[0].v != '"') return bad;
  char alg[8]; js_t aj = {hw[0].v, hw[0].ve}; long al = jstr(&aj, alg, sizeof alg);
  if (al != 5 || memcmp(alg, "HS256", 5)) return bad;
  want_t pw[4] = {{"exp"}, {"nbf"}, {"sub"}, {"username"}};
  if (!jdoc((char *)pl, pn, pw, 4) || !pw[0].v) return bad;  // exp is required
  for (int i = 0; i < 2; i++) if (pw[i].v) {  // exp / nbf must be numbers, checked against now
    if (*pw[i].v != '-' && (*pw[i].v < '0' || *pw[i].v > '9')) return bad;
    char nb[64]; size_t l = pw[i].ve - pw[i].v; if (l >= sizeof nb) return bad;
    memcpy(nb, pw[i].v, l); nb[l] = 0; double v = strtod(nb, NULL);
    if (i == 0 ? (double)now_s >= v : (double)now_s < v) return bad;
  }
  if (!pw[2].v || *pw[2].v != '"' || !pw[3].v || *pw[3].v != '"') return payload;
  char sb[24]; js_t sj = {pw[2].v, pw[2].ve}; long sl = jstr(&sj, sb, sizeof sb);
  if (sl <= 0 || sl > 15) return payload;
  int64_t v = 0; for (long i = 0; i < sl; i++) { if (sb[i] < '0' || sb[i] > '9') return payload; v = v * 10 + sb[i] - '0'; }
  if (v <= 0) return payload;
  js_t uj = {pw[3].v, pw[3].ve}; long ul = jstr(&uj, uname, 1024);
  if (ul < 0) return payload;
  *uid = v; *unl = ul; return NULL;
}

// ---------- Postgres: a few pipelined connections per thread ----------
// Every request runs one prepared statement. Reads queue up behind each other on a connection and share
// one Sync at the end of the loop iteration; each write gets its own Sync right after it, so it commits
// (or fails, e.g. a like's foreign key) on its own, and its response is sent only once that Sync's
// result arrives, i.e. after the commit.
enum { Q_FEED, Q_POST, Q_INS, Q_LIKE, Q_HEALTH, Q_SYNC };
typedef struct { int fd; uint32_t gen; uint8_t kind, close, gz; int64_t id; char *extra; uint32_t blen, ulen; } q_t;
typedef struct {
  PGconn *c; int sock;
  q_t *q; uint32_t head, n, cap;  // ring of sent statements and Syncs, in order
  uint8_t got, unsynced, dirty, wout; uint16_t inbatch;  // inbatch: statements since the last flush
  q_t held; PGresult *hres; uint8_t has_held, held_fail;  // a write's result, waiting for its commit
} pg_t;
typedef struct { int ep, lfd[2], nl, npg; pg_t *pg; time_t retry_at, paused_until; } th_t;

static const char *dburl; static int nthreads, npg_per;
static const char *g_host; static int g_ports[2], g_nports;
static const char *cols = "SELECT p.id, p.body, p.created_at, u.username, "
  "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id";

#define TAG_LISTEN (1ULL << 32)
#define TAG_PG (2ULL << 32)
static void ep_set(int ep, int op, int fd, uint64_t tag, uint32_t ev) { struct epoll_event e = {.events = ev, .data.u64 = tag}; epoll_ctl(ep, op, fd, &e); }

static int pg_connect(th_t *t, int i) {
  pg_t *p = &t->pg[i];
  static const char *const kw[] = {"dbname", "connect_timeout", NULL};
  const char *const val[] = {dburl, "3", NULL};  // a down database stalls this loop for at most 3 s a try
  PGconn *c = PQconnectdbParams(kw, val, 1);
  if (PQstatus(c) != CONNECTION_OK) { fprintf(stderr, "connect: %s", PQerrorMessage(c)); PQfinish(c); return 0; }
  char sql[512]; static const Oid i8[] = {20, 20}, i8t[] = {20, 25};
  struct { const char *name, *sql; int np; const Oid *types; } st[] = {
    {"f", NULL, 0, NULL}, {"p", NULL, 1, i8},
    {"i", "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at", 2, i8t},
    {"l", "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING", 2, i8},
    {"h", "SELECT 1", 0, NULL}};
  for (int k = 0; k < 5; k++) {
    if (k < 2) { snprintf(sql, sizeof sql, k ? "%s WHERE p.id = $1" : "%s ORDER BY p.created_at DESC, p.id DESC LIMIT 20", cols); st[k].sql = sql; }
    PGresult *r = PQprepare(c, st[k].name, st[k].sql, st[k].np, st[k].types);
    int ok = PQresultStatus(r) == PGRES_COMMAND_OK; PQclear(r);
    if (!ok) { fprintf(stderr, "prepare: %s", PQerrorMessage(c)); PQfinish(c); return 0; }
  }
  if (!PQenterPipelineMode(c) || PQsetnonblocking(c, 1)) { PQfinish(c); return 0; }
  p->c = c; p->sock = PQsocket(c);
  ep_set(t->ep, EPOLL_CTL_ADD, p->sock, TAG_PG | i, EPOLLIN);
  return 1;
}

static q_t *qpush(pg_t *p) {
  if (p->n == p->cap) {
    uint32_t nc = p->cap ? p->cap * 2 : 256; q_t *nq = malloc(nc * sizeof *nq);
    for (uint32_t k = 0; k < p->n; k++) nq[k] = p->q[(p->head + k) & (p->cap - 1)];
    free(p->q); p->q = nq; p->cap = nc; p->head = 0;
  }
  return &p->q[(p->head + p->n++) & (p->cap - 1)];
}

static void respond(th_t *t, q_t *q, PGresult *r, int fail);
static void pg_fail(th_t *t, pg_t *p) {  // the connection is gone: every request on it gets a 500
  fprintf(stderr, "postgres connection lost: %s", PQerrorMessage(p->c));
  epoll_ctl(t->ep, EPOLL_CTL_DEL, p->sock, NULL);
  PQfinish(p->c); p->c = NULL;
  if (p->has_held) { p->has_held = 0; PQclear(p->hres); respond(t, &p->held, NULL, 1); }
  while (p->n) {
    q_t q = p->q[p->head]; p->head = (p->head + 1) & (p->cap - 1); p->n--;
    if (q.kind != Q_SYNC) respond(t, &q, NULL, 1);
  }
  p->got = p->unsynced = p->dirty = p->wout = 0;
}

// Reads fill one connection's batch for this loop iteration (up to PACK statements), so a batch is one
// write, one round trip and one Sync on one connection; the next batch goes to the connection with the
// shortest queue. Writes get their own connections (the last quarter), spread by queue length, so a slow
// commit never holds up the reads pipelined behind it.
#define PACK 64
static pg_t *pick(th_t *t, int write) {
  pg_t *b = NULL; int nw = t->npg / 4 ? t->npg / 4 : 1;
  for (int i = 0; i < t->npg; i++) {
    pg_t *p = &t->pg[i];
    if (!p->c && t->retry_at != now_s) { t->retry_at = now_s; pg_connect(t, i); }  // reconnect one lost connection a second
    if (!p->c || (i >= t->npg - nw) != write) continue;
    if (!write && p->dirty && p->inbatch < PACK) return p;
    if (!b || p->n < b->n) b = p;
  }
  return b;
}

static void pg_sync(pg_t *p) {
  if (!PQsendPipelineSync(p->c)) return;
  q_t *q = qpush(p); memset(q, 0, sizeof *q); q->kind = Q_SYNC; p->unsynced = 0;
}

// Sends one statement. Returns 0 if no connection is available (the caller answers 500/503).
static int enqueue(th_t *t, int fd, int kind, int close, int64_t id, int64_t uid, const char *s, size_t sl, const char *un, size_t unl) {
  pg_t *p = pick(t, kind == Q_INS || kind == Q_LIKE);
  if (!p) return 0;
  uint64_t a = __builtin_bswap64(uid), b = __builtin_bswap64(id);
  const char *v[2]; int len[2] = {8, 8}, fmt[2] = {1, 1}, ok;
  switch (kind) {
    case Q_FEED: ok = PQsendQueryPrepared(p->c, "f", 0, NULL, NULL, NULL, 1); break;
    case Q_HEALTH: ok = PQsendQueryPrepared(p->c, "h", 0, NULL, NULL, NULL, 1); break;
    case Q_POST: v[0] = (char *)&b; ok = PQsendQueryPrepared(p->c, "p", 1, v, len, fmt, 1); break;
    case Q_LIKE: v[0] = (char *)&a; v[1] = (char *)&b; ok = PQsendQueryPrepared(p->c, "l", 2, v, len, fmt, 1); break;
    default: v[0] = (char *)&a; v[1] = s; len[1] = sl; ok = PQsendQueryPrepared(p->c, "i", 2, v, len, fmt, 1);
  }
  if (!ok) return 0;
  q_t *q = qpush(p);
  *q = (q_t){fd, __atomic_load_n(&conns[fd].gen, __ATOMIC_RELAXED), kind, close, want_gz, id, NULL, 0, 0};
  if (kind == Q_INS) {  // the response echoes the body and the token's username
    q->extra = malloc(sl + unl + 1); memcpy(q->extra, s, sl); memcpy(q->extra + sl, un, unl);
    q->blen = sl; q->ulen = unl;
  }
  p->dirty = 1; p->unsynced = 1; p->inbatch++;
  if (kind == Q_INS || kind == Q_LIKE) pg_sync(p);
  return 1;
}

static void pg_flush(th_t *t, pg_t *p) {
  int r = PQflush(p->c);
  if (r < 0) { pg_fail(t, p); return; }
  if (r != p->wout) { p->wout = r; ep_set(t->ep, EPOLL_CTL_MOD, p->sock, TAG_PG | (p - t->pg), EPOLLIN | (r ? EPOLLOUT : 0)); }
}

static inline int64_t be64(const char *p) { int64_t v; memcpy(&v, p, 8); return __builtin_bswap64(v); }
static inline int32_t be32(const char *p) { int32_t v; memcpy(&v, p, 4); return __builtin_bswap32(v); }
static void put_post_row(PGresult *r, int i) {
  PUTS("{\"id\":"); putu(be64(PQgetvalue(r, i, 0)));
  PUTS(",\"body\":\""); putjs((unsigned char *)PQgetvalue(r, i, 1), PQgetlength(r, i, 1));
  PUTS("\",\"created_at\":\""); put_ts(be64(PQgetvalue(r, i, 2)));
  PUTS("\",\"author\":\""); putjs((unsigned char *)PQgetvalue(r, i, 3), PQgetlength(r, i, 3));
  PUTS("\",\"like_count\":"); putu(be32(PQgetvalue(r, i, 4))); PUTS("}");
}
static int rows_ok(PGresult *r, int cols) { return r && PQresultStatus(r) == PGRES_TUPLES_OK && PQnfields(r) == cols; }

// ---------- connections ----------
static void conn_close(th_t *t, int fd) {
  conn_t *c = &conns[fd];
  free(c->in); free(c->out);
  uint32_t g = c->gen + 1; memset(c, 0, sizeof *c); __atomic_store_n(&c->gen, g, __ATOMIC_RELAXED);
  close(fd);  // also leaves the epoll set
}
static void process(th_t *t, int fd, char *buf, size_t n);
// Sends arena[0..alen) on fd: whatever doesn't fit in the socket buffer waits for EPOLLOUT.
static void send_resp(th_t *t, int fd, int close) {
  conn_t *c = &conns[fd];
  struct iovec iv[2] = {{arena, alen}, {zbuf, zlen}};
  size_t total = alen + zlen;
  ssize_t w = writev(fd, iv, zlen ? 2 : 1);
  if (w < 0) { if (errno != EAGAIN) { conn_close(t, fd); return; } w = 0; }
  if (close) c->closing = 1;
  if ((size_t)w < total) {
    c->outlen = total - w; c->outoff = 0; c->out = malloc(c->outlen);
    if ((size_t)w < alen) { memcpy(c->out, arena + w, alen - w); memcpy(c->out + alen - w, zbuf, zlen); }
    else memcpy(c->out, zbuf + (w - alen), c->outlen);
    ep_set(t->ep, EPOLL_CTL_MOD, fd, fd, EPOLLOUT);
  } else if (close) conn_close(t, fd);
}

// Answers a request whose statement finished (or failed: fail=1), then serves any request queued behind it.
static void respond(th_t *t, q_t *q, PGresult *r, int fail) {
  int fd = q->fd; conn_t *c = &conns[fd];
  if (q->gen != __atomic_load_n(&c->gen, __ATOMIC_RELAXED) || !c->open) { free(q->extra); return; }  // client left
  int close = q->close; want_gz = q->gz;
  ExecStatusType s = r ? PQresultStatus(r) : PGRES_FATAL_ERROR;
  if (fail) err(500, "internal server error", close);
  else switch (q->kind) {
    case Q_FEED:
      if (!rows_ok(r, 5)) { err(500, "internal server error", close); break; }
      begin_resp(200, close); PUTS("{\"posts\":[");
      for (int i = 0, n = PQntuples(r); i < n; i++) { if (i) PUTS(","); put_post_row(r, i); }
      PUTS("]}"); finish(); break;
    case Q_POST:
      if (!rows_ok(r, 5)) err(500, "internal server error", close);
      else if (!PQntuples(r)) err(404, "post not found", close);
      else { begin_resp(200, close); PUTS("{\"post\":"); put_post_row(r, 0); PUTS("}"); finish(); }
      break;
    case Q_INS:
      if (!rows_ok(r, 2) || PQntuples(r) != 1) { err(500, "internal server error", close); break; }
      begin_resp(201, close);
      PUTS("{\"post\":{\"id\":"); putu(be64(PQgetvalue(r, 0, 0)));
      PUTS(",\"body\":\""); putjs((unsigned char *)q->extra, q->blen);
      PUTS("\",\"created_at\":\""); put_ts(be64(PQgetvalue(r, 0, 1)));
      PUTS("\",\"author\":\""); putjs((unsigned char *)q->extra + q->blen, q->ulen); PUTS("\",\"like_count\":0}}"); finish();
      break;
    case Q_LIKE:
      if (s == PGRES_COMMAND_OK) {
        int created = !strcmp(PQcmdTuples(r), "1");
        begin_resp(created ? 201 : 200, close);
        PUTS("{\"liked\":true,\"already_liked\":"); if (created) PUTS("false"); else PUTS("true");
        PUTS(",\"post_id\":"); putu(q->id); PUTS("}"); finish();
      } else {
        const char *st = r ? PQresultErrorField(r, PG_DIAG_SQLSTATE) : NULL;
        if (st && !strcmp(st, "23503")) err(404, "post not found", close); else err(500, "internal server error", close);
      }
      break;
    case Q_HEALTH:
      if (s == PGRES_TUPLES_OK) { begin_resp(200, close); PUTS("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":"); putu(now_s - started); PUTS("}"); finish(); }
      else {
        const char *m = r ? PQresultErrorMessage(r) : "connection lost";
        begin_resp(503, close); PUTS("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\""); putjs((const unsigned char *)m, strlen(m)); PUTS("\"}"); finish();
      }
      break;
  }
  free(q->extra);
  c->busy = 0;
  send_resp(t, fd, close);
  if (c->open && !c->out && !c->closing && c->inlen) process(t, fd, c->in, c->inlen);
}

// Reads every result that has arrived. Statements and Syncs come back in the order they were sent.
static void pg_read(th_t *t, pg_t *p) {
  if (!PQconsumeInput(p->c)) { pg_fail(t, p); return; }
  while (p->c && p->n && !PQisBusy(p->c)) {
    q_t *q = &p->q[p->head];
    PGresult *r = PQgetResult(p->c);
    if (q->kind == Q_SYNC) {
      if (!r) break;
      if (PQresultStatus(r) != PGRES_PIPELINE_SYNC) { p->held_fail = 1; PQclear(r); continue; }  // the commit failed
      PQclear(r);
      p->head = (p->head + 1) & (p->cap - 1); p->n--;
      if (p->has_held) {  // committed: now the write may be answered
        q_t h = p->held; PGresult *hr = p->hres; int f = p->held_fail;
        p->has_held = p->held_fail = 0;
        respond(t, &h, hr, f); PQclear(hr);
      }
      continue;
    }
    if (p->got) {  // the NULL that ends this statement's results
      if (r) { PQclear(r); continue; }
      p->got = 0; p->head = (p->head + 1) & (p->cap - 1); p->n--;
      continue;
    }
    p->got = 1;
    q_t cq = *q;
    if (cq.kind == Q_INS || cq.kind == Q_LIKE) { p->held = cq; p->hres = r; p->has_held = 1; p->held_fail = 0; continue; }
    respond(t, &cq, r, r == NULL); PQclear(r);
  }
}

// ---------- request handling ----------
static int64_t parse_id(const char *s, size_t n) {  // positive decimal integer, else -1 (huge ids map to 0: not found)
  if (!n) return -1;
  int64_t v = 0;
  for (size_t i = 0; i < n; i++) { if (s[i] < '0' || s[i] > '9') return -1; if (v < 100000000000000000LL) v = v * 10 + s[i] - '0'; else v = 0x7fffffffffffffffLL; }
  return v > 0 ? v : -1;
}
static int jsws_at(const unsigned char *p, const unsigned char *e) {  // byte length of JS String.trim whitespace at p, or 0
  if (*p == ' ' || (*p >= 9 && *p <= 13)) return 1;
  if (e - p >= 2 && p[0] == 0xc2 && p[1] == 0xa0) return 2;
  if (e - p >= 3) {
    uint32_t c = p[0] << 16 | p[1] << 8 | p[2];
    if (c == 0xe19a80 || (c >= 0xe28080 && c <= 0xe2808a) || c == 0xe280a8 || c == 0xe280a9 || c == 0xe280af || c == 0xe2819f || c == 0xe38080 || c == 0xefbbbf) return 3;
  }
  return 0;
}
static int jsws_before(const unsigned char *s, const unsigned char *e) {
  if (e[-1] == ' ' || (e[-1] >= 9 && e[-1] <= 13)) return 1;
  if (e - s >= 2 && e[-2] == 0xc2 && e[-1] == 0xa0) return 2;
  if (e - s >= 3 && jsws_at(e - 3, e) == 3) return 3;
  return 0;
}
// Answers into the arena, or sends a statement and marks the connection busy.
static void handle(th_t *t, int fd, const char *m, size_t ml, const char *path, size_t pl, const char *authh, size_t al,
                   const char *body, size_t bl, int close) {
  const char *q = memchr(path, '?', pl); if (q) pl = q - path;
  int get = ml == 3 && !memcmp(m, "GET", 3), post = ml == 4 && !memcmp(m, "POST", 4), kind = -1;
  int64_t id = 0, uid = 0; char un[1024]; size_t unl = 0; const unsigned char *s = NULL, *se = NULL;
  static __thread char txt[MAXREQ];
  if (get && pl == 5 && !memcmp(path, "/feed", 5)) kind = Q_FEED;
  else if (get && pl == 7 && !memcmp(path, "/health", 7)) kind = Q_HEALTH;
  else if (pl > 7 && !memcmp(path, "/posts/", 7)) {
    const char *seg = path + 7, *sl = memchr(seg, '/', pl - 7);
    if (get && !sl) {
      if ((id = parse_id(seg, pl - 7)) < 0) { err(400, "invalid post id", close); return; }
      kind = Q_POST;
    } else if (post && sl && (size_t)(path + pl - sl) == 5 && !memcmp(sl, "/like", 5)) {
      const char *e = auth(authh, al, &uid, un, &unl);
      if (e) { err(401, e, close); return; }
      if ((id = parse_id(seg, sl - seg)) < 0) { err(400, "invalid post id", close); return; }
      kind = Q_LIKE;
    }
  } else if (post && pl == 6 && !memcmp(path, "/posts", 6)) {
    const char *e = auth(authh, al, &uid, un, &unl);
    if (e) { err(401, e, close); return; }
    want_t w[1] = {{"body"}};
    if (!jdoc(body, bl, w, 1)) { err(400, "malformed JSON body", close); return; }
    if (!w[0].v || *w[0].v != '"') { err(400, "body is required", close); return; }
    js_t bj = {w[0].v, w[0].ve};
    long n = jstr(&bj, txt, sizeof txt);
    if (n < 0) { err(400, "malformed JSON body", close); return; }
    s = (unsigned char *)txt; se = s + n; int l;
    while (s < se && (l = jsws_at(s, se))) s += l;
    while (se > s && (l = jsws_before(s, se))) se -= l;
    if (s == se) { err(400, "body is required", close); return; }
    size_t chars = 0; for (const unsigned char *c = s; c < se; c++) chars += (*c & 0xc0) != 0x80;
    if (chars > 500) { err(400, "body must be at most 500 characters", close); return; }
    kind = Q_INS;
  }
  if (kind < 0) { err(404, "not found", close); return; }
  if (enqueue(t, fd, kind, close, id, uid, (const char *)s, se - s, un, unl)) { conns[fd].busy = 1; return; }
  if (kind != Q_HEALTH) { err(500, "internal server error", close); return; }
  begin_resp(503, close); PUTS("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"no database connection\"}"); finish();
}

static int accepts_gzip(const char *v, const char *ve) {  // "gzip" listed, and not with q=0
  const char *g = memmem(v, ve - v, "gzip", 4);
  if (!g) return 0;
  for (g += 4; g < ve && *g == ' '; g++) {}
  if (g + 2 < ve && *g == ';' && (g = memmem(g, ve - g, "q=", 2))) { char b[8] = {0}; memcpy(b, g + 2, ve - g - 2 < 7 ? ve - g - 2 : 7); return strtod(b, NULL) > 0; }
  return 1;
}
static int hdr_is(const char *p, size_t n, const char *name, size_t nl) { return n == nl && !strncasecmp(p, name, nl); }

// Serves complete requests in buf until one waits for Postgres. Returns bytes consumed, or -1 to drop the connection.
static long serve(th_t *t, int fd, char *buf, size_t n) {
  conn_t *c = &conns[fd]; size_t used = 0;
  while (used < n && c->open && !c->busy && !c->closing && !c->out) {
    char *p = buf + used, *e = buf + n;
    char *he = memmem(p, e - p, "\r\n\r\n", 4);
    if (!he) { if ((size_t)(e - p) > MAXREQ) return -1; break; }
    char *le = memchr(p, '\r', he + 2 - p);
    char *m = p, *sp1 = memchr(m, ' ', le - m); if (!sp1) return -1;
    char *path = sp1 + 1, *sp2 = memchr(path, ' ', le - path); if (!sp2) return -1;
    int http10 = le - sp2 - 1 == 8 && !memcmp(sp2 + 1, "HTTP/1.0", 8);
    const char *authh = NULL; size_t al = 0, clen = 0; int close = http10, expect = 0; want_gz = 0;
    for (char *h = le + 2; h < he + 2;) {
      char *hl = memchr(h, '\r', he + 2 - h), *col = memchr(h, ':', hl - h);
      if (col) {
        char *v = col + 1; while (v < hl && (*v == ' ' || *v == '\t')) v++;
        char *ve = hl; while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        size_t kn = col - h;
        if (hdr_is(h, kn, "content-length", 14)) { clen = 0; for (char *d = v; d < ve; d++) { if (*d < '0' || *d > '9') return -1; clen = clen * 10 + *d - '0'; if (clen > MAXREQ) return -1; } }
        else if (hdr_is(h, kn, "authorization", 13)) { authh = v; al = ve - v; }
        else if (hdr_is(h, kn, "connection", 10)) { if (ve - v == 5 && !strncasecmp(v, "close", 5)) close = 1; else if (ve - v == 10 && !strncasecmp(v, "keep-alive", 10)) close = 0; }
        else if (hdr_is(h, kn, "accept-encoding", 15)) want_gz = accepts_gzip(v, ve);
        else if (hdr_is(h, kn, "expect", 6)) expect = ve - v == 12 && !strncasecmp(v, "100-continue", 12);
        else if (hdr_is(h, kn, "transfer-encoding", 17)) return -1;  // chunked uploads aren't supported
      }
      h = hl + 2;
    }
    char *body = he + 4;
    if ((size_t)(e - body) < clen) {
      if (expect && body == e) { static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n"; if (write(fd, cont, sizeof cont - 1) < 0) return -1; }
      break;
    }
    used = body + clen - buf;
    handle(t, fd, m, sp1 - m, path, sp2 - path, authh, al, body, clen, close);
    if (!c->busy) send_resp(t, fd, close);
  }
  return used;
}

static void process(th_t *t, int fd, char *buf, size_t n) {
  conn_t *c = &conns[fd];
  long used = serve(t, fd, buf, n);
  if (!c->open) return;  // closed while serving (Connection: close); buf may be freed
  if (used < 0) { conn_close(t, fd); return; }
  size_t left = c->closing ? 0 : n - used;
  if (left) {
    if (buf != c->in) { c->incap = left + RBUF; c->in = malloc(c->incap); memcpy(c->in, buf + used, left); }
    else if (used) memmove(c->in, c->in + used, left);
    c->inlen = left;
  } else if (c->in) { free(c->in); c->in = NULL; c->inlen = c->incap = 0; }
}

static __thread char rbuf[RBUF];
static void on_readable(th_t *t, int fd) {
  conn_t *c = &conns[fd];
  ssize_t r;
  if (c->inlen || c->busy) {  // append to the carried bytes; parse them only if nothing is in flight
    if (c->incap - c->inlen < 4096) {
      if (c->incap > 4 * MAXREQ) { conn_close(t, fd); return; }
      c->incap = c->incap * 2 + RBUF; c->in = realloc(c->in, c->incap);
    }
    r = read(fd, c->in + c->inlen, c->incap - c->inlen);
    if (r <= 0) { if (r < 0 && errno == EAGAIN) return; conn_close(t, fd); return; }
    c->inlen += r;
    if (!c->busy) process(t, fd, c->in, c->inlen);
  } else {
    r = read(fd, rbuf, RBUF);
    if (r <= 0) { if (r < 0 && errno == EAGAIN) return; conn_close(t, fd); return; }
    process(t, fd, rbuf, r);
  }
}
static void on_writable(th_t *t, int fd) {
  conn_t *c = &conns[fd];
  if (!c->out) return;
  ssize_t w = write(fd, c->out + c->outoff, c->outlen - c->outoff);
  if (w < 0) { if (errno == EAGAIN) return; conn_close(t, fd); return; }
  c->outoff += w;
  if (c->outoff < c->outlen) return;
  free(c->out); c->out = NULL; c->outlen = c->outoff = 0;
  if (c->closing) { conn_close(t, fd); return; }
  ep_set(t->ep, EPOLL_CTL_MOD, fd, fd, EPOLLIN);
  if (c->inlen && !c->busy) process(t, fd, c->in, c->inlen);
}

static int listen_on(int port) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0), one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);  // inherited by accepted sockets
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
  if (inet_pton(AF_INET, g_host, &a.sin_addr) != 1) { fprintf(stderr, "bad HOST %s\n", g_host); exit(1); }
  if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 65535)) { perror("bind/listen"); exit(1); }
  return fd;
}
static void listeners(th_t *t, int op) { for (int i = 0; i < t->nl; i++) ep_set(t->ep, op, t->lfd[i], TAG_LISTEN | i, EPOLLIN); }
static void accept_conns(th_t *t, int lfd) {
  for (int k = 0; k < 256; k++) {
    int c = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (c < 0) {
      if (errno == EMFILE || errno == ENFILE) { listeners(t, EPOLL_CTL_DEL); t->paused_until = now_s + 1; }  // out of descriptors: retry in a second
      return;
    }
    if (c >= maxfd) { close(c); continue; }
    conns[c].open = 1;
    ep_set(t->ep, EPOLL_CTL_ADD, c, c, EPOLLIN);
  }
}

static void *run(void *arg) {
  th_t *t = arg;
  arena = malloc(OBUF); zbuf = malloc(OBUF);
  now_s = time(NULL);
  for (int i = 0; i < t->npg; i++) pg_connect(t, i);
  for (int i = 0; i < g_nports; i++) t->lfd[t->nl++] = listen_on(g_ports[i]);
  listeners(t, EPOLL_CTL_ADD);
  struct epoll_event evs[512];
  for (;;) {
    int n = epoll_wait(t->ep, evs, 512, 1000);
    if (n < 0) { if (errno == EINTR) continue; perror("epoll_wait"); exit(1); }
    now_s = time(NULL);
    for (int i = 0; i < n; i++) {
      uint64_t tag = evs[i].data.u64; uint32_t e = evs[i].events; int id = (int)tag;
      if (tag >= TAG_PG) {
        pg_t *p = &t->pg[id];
        if (!p->c) continue;
        if (e & (EPOLLIN | EPOLLHUP | EPOLLERR)) pg_read(t, p);
        if (p->c && (e & EPOLLOUT)) p->dirty = 1;
      } else if (tag >= TAG_LISTEN) accept_conns(t, t->lfd[id]);
      else if (conns[id].open) {
        if (e & EPOLLOUT) on_writable(t, id);
        else if (e & (EPOLLIN | EPOLLHUP | EPOLLERR)) on_readable(t, id);
      }
    }
    for (int i = 0; i < t->npg; i++) {  // one Sync for this iteration's reads, then send everything
      pg_t *p = &t->pg[i];
      if (!p->c || !p->dirty) continue;
      if (p->unsynced) pg_sync(p);
      p->dirty = 0; p->inbatch = 0; pg_flush(t, p);
    }
    if (t->paused_until && now_s >= t->paused_until) { t->paused_until = 0; listeners(t, EPOLL_CTL_ADD); }
    // A light iteration: let events pile up for a moment, so the next one serves more per wake-up and per Sync.
    if (n && n < BATCH_MIN) { struct timespec ts = {0, BATCH_NS}; nanosleep(&ts, NULL); }
  }
  return NULL;
}

int main(void) {
  const char *secret = getenv("JWT_SECRET"), *port = getenv("PORT"), *extra = getenv("EXTRA_PORT");
  dburl = getenv("DATABASE_URL");
  if (!dburl || !secret) { fprintf(stderr, "DATABASE_URL and JWT_SECRET are required\n"); return 1; }
  g_host = getenv("HOST") ? getenv("HOST") : "0.0.0.0";
  g_ports[g_nports++] = port ? atoi(port) : 3000;
  if (extra && *extra) g_ports[g_nports++] = atoi(extra);
  signal(SIGPIPE, SIG_IGN);
  struct rlimit rl; getrlimit(RLIMIT_NOFILE, &rl);
  if (rl.rlim_cur < rl.rlim_max) { rl.rlim_cur = rl.rlim_max; setrlimit(RLIMIT_NOFILE, &rl); }
  maxfd = rl.rlim_cur > (16 << 20) ? (16 << 20) : (long)rl.rlim_cur;  // LimitNOFILE=8388608 on the box: 8.4M connections
  conns = calloc(maxfd, sizeof *conns);  // untouched pages cost nothing
  memset(b64t, -1, sizeof b64t);
  for (int i = 0; i < 64; i++) b64t[(unsigned char)"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"[i]] = i;
  hmac_key(secret);
  started = time(NULL);
  cpu_set_t cs; sched_getaffinity(0, sizeof cs, &cs);
  nthreads = CPU_COUNT(&cs);
  npg_per = 64 / nthreads > 2 ? 64 / nthreads : 2;  // ~64 in all; reads and writes need one each
  fprintf(stderr, "%d threads, %d Postgres connections each, ports %d%s\n", nthreads, npg_per, g_ports[0], g_nports > 1 ? " + extra" : "");
  th_t *ts = calloc(nthreads, sizeof *ts);
  for (int i = 0; i < nthreads; i++) {
    ts[i].ep = epoll_create1(EPOLL_CLOEXEC); ts[i].npg = npg_per; ts[i].pg = calloc(npg_per, sizeof(pg_t));
    if (i) { pthread_t th; pthread_create(&th, NULL, run, &ts[i]); }
  }
  run(&ts[0]);
}
