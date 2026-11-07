// Feed API on Postgres: one epoll loop per CPU, each with its own SO_REUSEPORT listeners on PORT and
// EXTRA_PORT and its own Postgres connections, spoken to in the v3 wire protocol directly. See README.md.
// Config comes only from DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
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
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include "igzip_lib.h"

#define RBUF (64 * 1024)
#define MAXREQ (64 * 1024)  // header + body cap per request
#define OBUF (256 * 1024)   // one response; the largest (20 posts of 500 escaped chars) is ~70 KB
#define GZMIN 1024          // smaller bodies (everything but the feed) go out uncompressed
#define NEV 512             // epoll events per pass; a pass that did not fill this array ...
#define BATCH_NS 2000000    // ... lasts at least 2 ms, the rest of it slept (see README.md, "Batching window")

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
static __thread int64_t now_ns;  // CLOCK_MONOTONIC at the start of the loop iteration
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
static void hmac_raw(const void *key, size_t kl, const void *m, size_t n, unsigned char *out) {  // any key (SCRAM)
  unsigned char k[64] = {0}, pad[64], ih[32]; sha_t s;
  if (kl > 64) { sha_init(&s); sha_upd(&s, key, kl); sha_fin(&s, k); } else memcpy(k, key, kl);
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
  sha_init(&s); sha_upd(&s, pad, 64); sha_upd(&s, m, n); sha_fin(&s, ih);
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
  sha_init(&s); sha_upd(&s, pad, 64); sha_upd(&s, ih, 32); sha_fin(&s, out);
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
// standard alphabet with padding, as SCRAM uses it
static void b64enc(const unsigned char *s, int n, char *o) {
  static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (int i = 0; i < n; i += 3) {
    uint32_t v = s[i] << 16 | (i + 1 < n ? s[i + 1] << 8 : 0) | (i + 2 < n ? s[i + 2] : 0);
    *o++ = A[v >> 18]; *o++ = A[v >> 12 & 63]; *o++ = i + 1 < n ? A[v >> 6 & 63] : '='; *o++ = i + 2 < n ? A[v & 63] : '=';
  }
  *o = 0;
}
static int b64dec_std(const char *s, size_t n, unsigned char *out) {
  char t[128]; size_t m = 0;
  for (size_t i = 0; i < n && s[i] != '=' && m < sizeof t; i++) t[m++] = s[i] == '+' ? '-' : s[i] == '/' ? '_' : s[i];
  return b64dec(t, m, out);
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

// ---------- Postgres: a few pipelined connections per thread, v3 wire protocol ----------
// Every request runs one prepared statement: Bind (binary parameters and results) + Execute appended to the
// connection's output buffer, which goes out in one write at the end of the loop iteration after one Sync. No
// Describe, so a reply is BindComplete, DataRows, CommandComplete and nothing else (libpq's PQsendQueryPrepared
// always adds a Describe, i.e. a RowDescription of ~130 bytes on every reply). Statements queue up behind each
// other and share one Sync (group commit for the writes: one implicit transaction, one commit). A write's
// response is held until that Sync's ReadyForQuery arrives, i.e. after the commit. If a write fails (e.g. a
// like's foreign key), Postgres rolls the whole group back and skips the rest until the Sync: the failing write
// gets its error and the others are sent again, each alone with its own Sync (see pg_read).
enum { Q_FEED, Q_POST, Q_INS, Q_LIKE, Q_HEALTH, Q_SYNC };
enum { ST_OK, ST_ERR, ST_ABORT, ST_LOST };  // a statement's outcome: done / ErrorResponse / skipped after another's error / connection or commit lost
#define GROUP 128  // writes per Sync at most
// fd -1: a read that was sent again on another connection (see hedge), whose late reply here is dropped
typedef struct { int fd; uint32_t gen; uint8_t kind, close, gz; int64_t id, uid; char *extra; uint32_t blen, ulen, t_ms; } q_t;
typedef struct { uint8_t st; uint16_t n; char code[6]; int64_t id, ts; const char *msg; } res_t;  // n: rows (reads), created (like), good row (insert)
typedef struct {
  int sock;                             // -1 while down
  char *ob; uint32_t olen, ocap;        // Bind/Execute/Sync bytes not yet written
  char *ib; uint32_t ilen, icap, scan, done;  // replies: parsed up to scan, consumed up to done (the current statement's rows sit between)
  q_t *q; uint32_t head, n, cap;        // ring of sent statements and Syncs, in order
  uint8_t unsynced, dirty, wout; uint16_t inbatch;  // inbatch: statements since the last Sync
  res_t cur;                            // the statement at the head of the ring, so far
  q_t held[GROUP]; res_t hres[GROUP]; uint16_t nheld;  // this group's writes and results, waiting for its commit
  uint8_t gfail, cfail;  // a write in the group failed / the Sync itself failed
  uint8_t hedged;        // its reads were sent again elsewhere; it gets no new reads until it answers
  int64_t last_rx, wait_from;  // now_ns of the last reply bytes / of the first statement queued on an idle ring
} pg_t;
typedef struct { int ep, lfd[2], nl, npg; pg_t *pg; time_t retry_at, paused_until; } th_t;

static struct { char user[256], pass[256], host[256], port[8], db[256]; } db;  // from DATABASE_URL
static int nthreads, npg_per;
static const char *g_host; static int g_ports[2], g_nports;
static const char *cols = "SELECT p.id, p.body, p.created_at, u.username, "
  "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id";

#define TAG_LISTEN (1ULL << 32)
#define TAG_PG (2ULL << 32)
static void ep_set(int ep, int op, int fd, uint64_t tag, uint32_t ev) { struct epoll_event e = {.events = ev, .data.u64 = tag}; epoll_ctl(ep, op, fd, &e); }

static inline int64_t be64(const char *p) { int64_t v; memcpy(&v, p, 8); return __builtin_bswap64(v); }
static inline int32_t be32(const char *p) { int32_t v; memcpy(&v, p, 4); return __builtin_bswap32(v); }
static inline uint16_t be16(const char *p) { return (uint8_t)p[0] << 8 | (uint8_t)p[1]; }
static inline void w32(char *p, uint32_t v) { v = __builtin_bswap32(v); memcpy(p, &v, 4); }
static inline void w16(char *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }

// postgres://user:pass@host:port/db?...  (query parameters are ignored: the link is plain TCP)
static void pct(char *s) { char *o = s; for (; *s; s++, o++) { int a, b; if (*s == '%' && (a = hexv(s[1])) >= 0 && (b = hexv(s[2])) >= 0) { *o = a << 4 | b; s += 2; } else *o = *s; } *o = 0; }
static int parse_url(const char *u) {
  const char *r = strstr(u, "://"); if (!r) return 0; r += 3;
  const char *end = strchr(r, '?'); if (!end) end = r + strlen(r);
  const char *at = NULL; for (const char *x = r; x < end; x++) if (*x == '@') at = x;
  const char *hp = at ? at + 1 : r, *slash = memchr(hp, '/', end - hp), *he = slash ? slash : end;
  const char *colon = at ? memchr(r, ':', at - r) : NULL;
  snprintf(db.user, sizeof db.user, "%.*s", (int)((colon ? colon : at ? at : r) - r), r);
  if (colon) snprintf(db.pass, sizeof db.pass, "%.*s", (int)(at - colon - 1), colon + 1);
  const char *pc = NULL; for (const char *x = hp; x < he; x++) if (*x == ':') pc = x;
  if (*hp == '[' && pc && pc[-1] != ']') pc = NULL;  // [v6::addr] without a port
  snprintf(db.host, sizeof db.host, "%.*s", (int)((pc ? pc : he) - hp), hp);
  if (db.host[0] == '[') { memmove(db.host, db.host + 1, strlen(db.host)); char *b = strchr(db.host, ']'); if (b) *b = 0; }
  snprintf(db.port, sizeof db.port, "%.*s", pc ? (int)(he - pc - 1) : 4, pc ? pc + 1 : "5432");
  if (slash) snprintf(db.db, sizeof db.db, "%.*s", (int)(end - slash - 1), slash + 1);
  pct(db.user); pct(db.pass); pct(db.db);
  if (!db.user[0]) snprintf(db.user, sizeof db.user, "%s", getenv("USER") ? getenv("USER") : "postgres");
  if (!db.db[0]) strcpy(db.db, db.user);
  if (!db.host[0]) strcpy(db.host, "localhost");
  return 1;
}

// One field of an ErrorResponse ('C' SQLSTATE, 'M' message), NUL-terminated, or NULL.
static const char *efield(const char *p, uint32_t n, char want) {
  const char *e = p + n;
  while (p < e && *p) { const char *v = p + 1, *z = memchr(v, 0, e - v); if (!z) return NULL; if (*p == want) return v; p = z + 1; }
  return NULL;
}
static const char *emsg(const char *p, uint32_t n) { const char *m = efield(p, n, 'M'); return m ? m : "unknown error"; }

// Blocking I/O for the handshake (SO_RCVTIMEO/SO_SNDTIMEO are set to 3 s). rd_msg returns the type, 0 on failure.
static int io_n(int fd, char *b, size_t n, int wr) { while (n) { ssize_t r = wr ? write(fd, b, n) : read(fd, b, n); if (r <= 0) return -1; b += r; n -= r; } return 0; }
static int rd_msg(int fd, char *buf, uint32_t cap, uint32_t *n) {
  char h[5]; if (io_n(fd, h, 5, 0)) return 0;
  uint32_t len = be32(h + 1); if (len < 4 || len - 4 > cap) return 0;
  *n = len - 4; return io_n(fd, buf, *n, 0) ? 0 : h[0];
}

// SCRAM-SHA-256 (RFC 7677) client, no channel binding. m is a scratch buffer of cap bytes.
// shortcut: the password is not SASLprep'd (it is plain ASCII on the box and in CI); add it if a password with non-ASCII ever fails
static int scram(int fd, char *m, uint32_t cap) {
  unsigned char raw[18], salt[64], u[32], sp[32], ck[32], sk[32], stored[32], sig[32], proof[32]; uint32_t n;
  if (getrandom(raw, sizeof raw, 0) != sizeof raw) return 0;
  char nonce[25]; b64enc(raw, 18, nonce);
  char cfirst[48]; int cfl = snprintf(cfirst, sizeof cfirst, "n,,n=,r=%s", nonce);  // the "bare" part starts at +3
  m[0] = 'p'; w32(m + 1, 4 + 14 + 4 + cfl); memcpy(m + 5, "SCRAM-SHA-256", 14); w32(m + 19, cfl); memcpy(m + 23, cfirst, cfl);
  if (io_n(fd, m, 23 + cfl, 1)) return 0;
  int ty = rd_msg(fd, m, cap, &n);
  if (ty == 'E') { fprintf(stderr, "connect: %s\n", emsg(m, n)); return 0; }
  if (ty != 'R' || n < 4 || be32(m) != 11 || n - 4 >= 512) return 0;
  char sfirst[512]; memcpy(sfirst, m + 4, n - 4); sfirst[n - 4] = 0;
  const char *r = NULL, *s = NULL; unsigned iters = 0;
  for (const char *x = sfirst; x; x = (x = strchr(x, ',')) ? x + 1 : NULL) {
    if (!strncmp(x, "r=", 2)) r = x + 2; else if (!strncmp(x, "s=", 2)) s = x + 2; else if (!strncmp(x, "i=", 2)) iters = atoi(x + 2);
  }
  if (!r || !s || !iters || strncmp(r, nonce, 24)) { fprintf(stderr, "connect: bad SCRAM server-first\n"); return 0; }
  const char *re = strchrnul(r, ','), *se = strchrnul(s, ',');
  int sl = b64dec_std(s, se - s, salt); if (sl <= 0 || sl > 60) return 0;
  memcpy(salt + sl, "\0\0\0\1", 4);  // PBKDF2-HMAC-SHA256, one block
  size_t pl = strlen(db.pass);
  hmac_raw(db.pass, pl, salt, sl + 4, u); memcpy(sp, u, 32);
  for (unsigned k = 1; k < iters; k++) { hmac_raw(db.pass, pl, u, 32, u); for (int j = 0; j < 32; j++) sp[j] ^= u[j]; }
  hmac_raw(sp, 32, "Client Key", 10, ck); hmac_raw(sp, 32, "Server Key", 10, sk);
  sha_t h; sha_init(&h); sha_upd(&h, ck, 32); sha_fin(&h, stored);
  char cfinal[300]; int cfnl = snprintf(cfinal, sizeof cfinal, "c=biws,r=%.*s", (int)(re - r), r);
  char am[1024]; int aml = snprintf(am, sizeof am, "%s,%s,%s", cfirst + 3, sfirst, cfinal);
  if (cfnl >= (int)sizeof cfinal || aml >= (int)sizeof am) return 0;
  hmac_raw(stored, 32, am, aml, sig); for (int j = 0; j < 32; j++) proof[j] = ck[j] ^ sig[j];
  char pb[48]; b64enc(proof, 32, pb);
  int l = snprintf(m + 5, cap - 5, "%s,p=%s", cfinal, pb); m[0] = 'p'; w32(m + 1, l + 4);
  if (io_n(fd, m, l + 5, 1)) return 0;
  ty = rd_msg(fd, m, cap, &n);
  if (ty == 'E') { fprintf(stderr, "connect: %s\n", emsg(m, n)); return 0; }
  hmac_raw(sk, 32, am, aml, sig); char want[48] = "v="; b64enc(sig, 32, want + 2);
  if (ty != 'R' || n < 4 || be32(m) != 12 || n - 4 != strlen(want) || memcmp(m + 4, want, n - 4)) { fprintf(stderr, "connect: SCRAM server signature mismatch\n"); return 0; }
  return 1;
}

// Connects, authenticates (trust, cleartext or SCRAM-SHA-256), prepares the five statements, goes non-blocking.
static int pg_connect(th_t *t, int i) {
  pg_t *p = &t->pg[i]; static __thread char m[16384]; uint32_t n, l;
  struct addrinfo *ai, hints = {.ai_socktype = SOCK_STREAM};
  if (getaddrinfo(db.host, db.port, &hints, &ai)) { fprintf(stderr, "connect: cannot resolve %s\n", db.host); return 0; }
  int fd = socket(ai->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;
  struct timeval tmo = {3, 0};  // a down database stalls this loop for at most 3 s a try (connect honours SO_SNDTIMEO)
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof tmo); setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
  setsockopt(fd, IPPROTO_TCP, TCP_THIN_LINEAR_TIMEOUTS, &one, sizeof one);  // as on client sockets
  int ok = connect(fd, ai->ai_addr, ai->ai_addrlen) == 0; freeaddrinfo(ai);
  if (!ok) { fprintf(stderr, "connect: %s\n", strerror(errno)); close(fd); return 0; }
  l = 8 + snprintf(m + 8, sizeof m - 8, "user%c%s%cdatabase%c%s%c", 0, db.user, 0, 0, db.db, 0) + 1;  // StartupMessage; a last NUL ends the list
  m[l - 1] = 0; w32(m, l); w32(m + 4, 196608);
  if (io_n(fd, m, l, 1)) goto fail;
  for (;;) {  // authentication, then ParameterStatus/BackendKeyData until ReadyForQuery
    int ty = rd_msg(fd, m, sizeof m, &n);
    if (!ty) { fprintf(stderr, "connect: no reply\n"); goto fail; }
    if (ty == 'E') { fprintf(stderr, "connect: %s\n", emsg(m, n)); goto fail; }
    if (ty == 'Z') break;
    if (ty != 'R' || n < 4) continue;
    uint32_t code = be32(m);
    if (code == 0) continue;
    if (code == 3) { l = strlen(db.pass) + 1; char pw[300] = "p"; w32(pw + 1, l + 4); memcpy(pw + 5, db.pass, l); if (l > 290 || io_n(fd, pw, l + 5, 1)) goto fail; }
    else if (code == 10 && memmem(m + 4, n - 4, "SCRAM-SHA-256", 14)) { if (!scram(fd, m, sizeof m)) goto fail; }
    else { fprintf(stderr, "connect: unsupported authentication method %u (trust, password and scram-sha-256 are)\n", code); goto fail; }
  }
  static const uint32_t i8[] = {20, 20}, i8t[] = {20, 25};
  struct { char name; const char *sql; int np; const uint32_t *types; } st[] = {
    {'f', NULL, 0, NULL}, {'p', NULL, 1, i8},
    {'i', "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at", 2, i8t},
    {'l', "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING", 2, i8},
    {'h', "SELECT 1", 0, NULL}};
  char sql[2][512]; l = 0;
  for (int k = 0; k < 5; k++) {  // Parse x5, one Sync
    if (k < 2) { snprintf(sql[k], sizeof sql[k], k ? "%s WHERE p.id = $1" : "%s ORDER BY p.created_at DESC, p.id DESC LIMIT 20", cols); st[k].sql = sql[k]; }
    char *b = m + l; b[0] = 'P'; b[5] = st[k].name; b[6] = 0; size_t sl = strlen(st[k].sql) + 1; memcpy(b + 7, st[k].sql, sl);
    char *e = b + 7 + sl; w16(e, st[k].np); e += 2;
    for (int j = 0; j < st[k].np; j++, e += 4) w32(e, st[k].types[j]);
    w32(b + 1, e - b - 1); l = e - m;
  }
  memcpy(m + l, "S\0\0\0\4", 5); l += 5;
  if (io_n(fd, m, l, 1)) goto fail;
  for (;;) {
    int ty = rd_msg(fd, m, sizeof m, &n);
    if (!ty) { fprintf(stderr, "prepare: no reply\n"); goto fail; }
    if (ty == 'E') { fprintf(stderr, "prepare: %s\n", emsg(m, n)); goto fail; }
    if (ty == 'Z') break;
  }
  fcntl(fd, F_SETFL, O_NONBLOCK);
  p->sock = fd;
  if (!p->ib) { p->icap = 64 * 1024; p->ib = malloc(p->icap); }
  ep_set(t->ep, EPOLL_CTL_ADD, fd, TAG_PG | i, EPOLLIN);
  return 1;
fail:
  close(fd); return 0;
}

static q_t *qpush(pg_t *p) {
  if (p->n == p->cap) {
    uint32_t nc = p->cap ? p->cap * 2 : 256; q_t *nq = malloc(nc * sizeof *nq);
    for (uint32_t k = 0; k < p->n; k++) nq[k] = p->q[(p->head + k) & (p->cap - 1)];
    free(p->q); p->q = nq; p->cap = nc; p->head = 0;
  }
  return &p->q[(p->head + p->n++) & (p->cap - 1)];
}
static q_t qpop(pg_t *p) { q_t q = p->q[p->head]; p->head = (p->head + 1) & (p->cap - 1); p->n--; return q; }

static void respond(th_t *t, q_t *q, const res_t *r, const char *rows, uint32_t rn);
static void pg_fail(th_t *t, pg_t *p, const char *why) {  // the connection is gone: every request on it gets a 500
  fprintf(stderr, "postgres connection lost: %s\n", why);
  epoll_ctl(t->ep, EPOLL_CTL_DEL, p->sock, NULL);
  close(p->sock); p->sock = -1;
  t->retry_at = now_s;  // no reconnect from inside respond() below, which would queue new statements on this ring mid-drain
  res_t lost = {.st = ST_LOST};
  for (int k = 0; k < p->nheld; k++) respond(t, &p->held[k], &lost, NULL, 0);  // commit unknown
  p->nheld = 0;
  while (p->n) { q_t q = qpop(p); if (q.kind != Q_SYNC) respond(t, &q, &lost, NULL, 0); }
  p->unsynced = p->dirty = p->wout = p->gfail = p->cfail = p->hedged = 0; p->inbatch = 0;
  p->olen = p->ilen = p->scan = p->done = 0; memset(&p->cur, 0, sizeof p->cur);
}

// A loop iteration's reads fill one connection's batch (up to PACK statements) and its writes one write
// connection's group (up to GROUP), so a batch is one write, one round trip and one Sync on one connection;
// the next batch goes to the connection with the shortest queue. Writes get their own connections (the last
// quarter), so a slow commit never holds up the reads pipelined behind it.
#define PACK 64
static pg_t *pick(th_t *t, int write) {
  pg_t *b = NULL; int nw = t->npg / 4 ? t->npg / 4 : 1;
  for (int i = 0; i < t->npg; i++) {
    pg_t *p = &t->pg[i];
    if (p->sock < 0 && t->retry_at != now_s) { t->retry_at = now_s; pg_connect(t, i); }  // reconnect one lost connection a second
    if (p->sock < 0 || (i >= t->npg - nw) != write || p->hedged) continue;
    if (p->dirty && p->inbatch < (write ? GROUP : PACK)) return p;
    if (!b || p->n < b->n) b = p;
  }
  return b;
}

static char *ob_room(pg_t *p, uint32_t n) {
  if (p->olen + n > p->ocap) { p->ocap = (p->olen + n) * 2; p->ob = realloc(p->ob, p->ocap); }
  char *r = p->ob + p->olen; p->olen += n; return r;
}
static void pg_sync(pg_t *p) {
  memcpy(ob_room(p, 5), "S\0\0\0\4", 5);
  q_t *q = qpush(p); memset(q, 0, sizeof *q); q->kind = Q_SYNC; p->unsynced = 0; p->inbatch = 0;
}

// Bind (unnamed portal, binary parameters) + Execute for one statement. Results are binary, except a post's id
// and like_count, which come back as text: "523117" and "0" are 6 and 1 bytes where binary is 8 and 4, and
// Postgres rows are most of the bytes coming into the box (the feed: 20 rows, 100 bytes fewer).
static void send_stmt(pg_t *p, const q_t *q) {
  int rows = q->kind == Q_FEED || q->kind == Q_POST;
  uint64_t a = __builtin_bswap64(q->uid), b = __builtin_bswap64(q->id);
  const char *v[2] = {(char *)&a, (char *)&b}; uint32_t len[2] = {8, 8}; int np = 2;
  switch (q->kind) {
    case Q_FEED: case Q_HEALTH: np = 0; break;
    case Q_POST: v[0] = (char *)&b; np = 1; break;
    case Q_INS: v[1] = q->extra; len[1] = q->blen; break;
  }
  uint32_t bl = 1 + 2 + 2 + (np ? 2 : 0) + 2 + (rows ? 12 : 4);  // portal, name, nfmt, fmt, nparams, result formats
  for (int k = 0; k < np; k++) bl += 4 + len[k];
  char *m = ob_room(p, 5 + bl + 10), *e = m + 5;
  m[0] = 'B'; w32(m + 1, bl + 4);
  *e++ = 0; *e++ = "fpilh"[q->kind]; *e++ = 0;
  w16(e, np ? 1 : 0); e += 2; if (np) { w16(e, 1); e += 2; }
  w16(e, np); e += 2;
  for (int k = 0; k < np; k++) { w32(e, len[k]); memcpy(e + 4, v[k], len[k]); e += 4 + len[k]; }
  if (rows) { memcpy(e, "\0\5\0\0\0\1\0\1\0\1\0\0", 12); e += 12; }  // id text, body, created_at, username binary, like_count text
  else { memcpy(e, "\0\1\0\1", 4); e += 4; }
  memcpy(e, "E\0\0\0\x09\0\0\0\0\0", 10);
}

// Sends one statement. Returns 0 if no connection is available (the caller answers 500/503).
static int enqueue(th_t *t, int fd, int kind, int close, int64_t id, int64_t uid, const char *s, size_t sl, const char *un, size_t unl) {
  int write = kind == Q_INS || kind == Q_LIKE;
  pg_t *p = pick(t, write);
  if (!p) return 0;
  q_t q = {fd, __atomic_load_n(&conns[fd].gen, __ATOMIC_RELAXED), kind, close, want_gz, id, uid, NULL, 0, 0, now_ns / 1000000};
  if (!p->n) p->wait_from = now_ns;
  if (kind == Q_INS) {  // the response echoes the body and the token's username
    q.extra = malloc(sl + unl + 1); memcpy(q.extra, s, sl); memcpy(q.extra + sl, un, unl);
    q.blen = sl; q.ulen = unl;
  }
  send_stmt(p, &q);
  *qpush(p) = q;
  p->dirty = 1; p->unsynced = 1; p->inbatch++;
  if (write && p->inbatch == GROUP) pg_sync(p);  // a full group commits on its own; the rest of the pass starts the next
  return 1;
}

// Loss hedging for reads. A batch of up to 64 reads shares one round trip, so one lost packet on its Postgres
// connection (either way) stalls all of them until TCP retransmits: 200 ms at the least, and at the limit every
// run's p95 sits at ~220 ms, i.e. on that timer. A read connection that has had no reply bytes for HEDGE_NS while
// it owes replies, at a time another read connection of the thread did get replies (so Postgres is answering,
// not slow), has its outstanding reads sent again on the least-loaded other read connection. The copies answer
// the clients; the originals stay in the ring with fd -1 and their late replies are dropped. Writes are never
// sent twice. Each read still runs its own SPEC statement; a hedged one runs a second time, only after a stall.
#define HEDGE_NS 20000000
static unsigned long hedged_total;  // statements sent again, all threads (reported on stderr)
static void hedge(th_t *t) {
  int nr = t->npg - (t->npg / 4 ? t->npg / 4 : 1);  // read connections are the first nr
  for (int i = 0; i < nr; i++) {
    pg_t *p = &t->pg[i];
    if (p->sock < 0 || !p->n || p->hedged || now_ns - (p->last_rx > p->wait_from ? p->last_rx : p->wait_from) < HEDGE_NS) continue;
    pg_t *to = NULL; int answering = 0;
    for (int k = 0; k < nr; k++) {
      pg_t *o = &t->pg[k];
      if (o == p || o->sock < 0 || o->hedged) continue;
      if (now_ns - o->last_rx < HEDGE_NS) answering = 1;
      if (!to || o->n < to->n) to = o;
    }
    if (!answering || !to) continue;  // nobody answers: Postgres is slow, and copies would only add to its load
    p->hedged = 1;
    for (uint32_t k = 0; k < p->n; k++) {
      q_t *q = &p->q[(p->head + k) & (p->cap - 1)];
      if (q->kind == Q_SYNC || q->fd < 0) continue;
      q_t c = *q; q->fd = -1;
      if (!to->n) to->wait_from = now_ns;
      send_stmt(to, &c); *qpush(to) = c;
      to->dirty = 1; to->unsynced = 1; to->inbatch++;
      __atomic_add_fetch(&hedged_total, 1, __ATOMIC_RELAXED);
    }
  }
}

// Sends a write again, alone between two Syncs, after another write's error rolled its group back.
static void resend(th_t *t, pg_t *p, q_t *q) {
  if (q->gen != __atomic_load_n(&conns[q->fd].gen, __ATOMIC_RELAXED) || !conns[q->fd].open) { free(q->extra); return; }
  if (p->unsynced) pg_sync(p);  // close this pass's open group first, so the retry is not part of it
  send_stmt(p, q);
  *qpush(p) = *q; p->dirty = 1; pg_sync(p);
}

static void pg_flush(th_t *t, pg_t *p) {
  ssize_t w = p->olen ? write(p->sock, p->ob, p->olen) : 0;
  if (w < 0) { if (errno != EAGAIN) { pg_fail(t, p, strerror(errno)); return; } w = 0; }
  if ((uint32_t)w < p->olen) memmove(p->ob, p->ob + w, p->olen - w);  // socket buffer full: rare
  p->olen -= w;
  int r = p->olen > 0;
  if (r != p->wout) { p->wout = r; ep_set(t->ep, EPOLL_CTL_MOD, p->sock, TAG_PG | (p - t->pg), EPOLLIN | (r ? EPOLLOUT : 0)); }
}

// The columns of a DataRow payload; 0 if not n of them or one is NULL.
static int row_cols(const char *pl, uint32_t pn, int n, const char **v, int32_t *l) {
  if (pn < 2 || be16(pl) != n) return 0;
  const char *p = pl + 2, *e = pl + pn;
  for (int i = 0; i < n; i++) { if (e - p < 4) return 0; l[i] = be32(p); p += 4; if (l[i] < 0 || e - p < l[i]) return 0; v[i] = p; p += l[i]; }
  return 1;
}
static int put_post_row(const char *pl, uint32_t pn) {
  const char *v[5]; int32_t l[5];
  if (!row_cols(pl, pn, 5, v, l) || l[0] < 1 || l[0] > 19 || l[2] != 8 || l[4] < 1 || l[4] > 10) return 0;
  PUTS("{\"id\":"); put(v[0], l[0]);  // id and like_count arrive as decimal text (see send_stmt)
  PUTS(",\"body\":\""); putjs((const unsigned char *)v[1], l[1]);
  PUTS("\",\"created_at\":\""); put_ts(be64(v[2]));
  PUTS("\",\"author\":\""); putjs((const unsigned char *)v[3], l[3]);
  PUTS("\",\"like_count\":"); put(v[4], l[4]); PUTS("}");
  return 1;
}
// Writes the post objects of the DataRows in rows[0..rn) (other messages there are skipped), comma-separated,
// at most max of them. Returns -1 on a malformed row.
static int put_rows(const char *rows, uint32_t rn, int max) {
  int k = 0;
  for (uint32_t i = 0; i + 5 <= rn && k != max; ) {
    uint32_t len = be32(rows + i + 1);
    if (rows[i] == 'D') { if (k++) PUTS(","); if (!put_post_row(rows + i + 5, len - 4)) return -1; }
    i += 1 + len;
  }
  return k;
}

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

// Answers a request whose statement finished (r->st says how; rows[0..rn) are its DataRows), then serves any
// request queued behind it.
static void respond(th_t *t, q_t *q, const res_t *r, const char *rows, uint32_t rn) {
  if (q->fd < 0) return;  // hedged: its copy on another connection answers
  int fd = q->fd; conn_t *c = &conns[fd];
  if (q->gen != __atomic_load_n(&c->gen, __ATOMIC_RELAXED) || !c->open) { free(q->extra); return; }  // client left
  // A reply later than the 40 ms delayed-ACK timer made the kernel ACK the request alone and leave delayed-ACK
  // mode; going back into it lets the next request's ACK ride on its response again (one packet, not two).
  if ((uint32_t)(now_ns / 1000000) - q->t_ms > 30) { int zero = 0; setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &zero, sizeof zero); }
  int close = q->close; want_gz = q->gz;
  if (r->st == ST_LOST) err(500, "internal server error", close);
  else switch (q->kind) {
    case Q_FEED:
      if (r->st != ST_OK) { err(500, "internal server error", close); break; }
      begin_resp(200, close); PUTS("{\"posts\":[");
      if (put_rows(rows, rn, -1) < 0) { err(500, "internal server error", close); break; }
      PUTS("]}"); finish(); break;
    case Q_POST:
      if (r->st != ST_OK) err(500, "internal server error", close);
      else if (!r->n) err(404, "post not found", close);
      else { begin_resp(200, close); PUTS("{\"post\":"); if (put_rows(rows, rn, 1) < 0) { err(500, "internal server error", close); break; } PUTS("}"); finish(); }
      break;
    case Q_INS:
      if (r->st != ST_OK || !r->n) { err(500, "internal server error", close); break; }
      begin_resp(201, close);
      PUTS("{\"post\":{\"id\":"); putu(r->id);
      PUTS(",\"body\":\""); putjs((unsigned char *)q->extra, q->blen);
      PUTS("\",\"created_at\":\""); put_ts(r->ts);
      PUTS("\",\"author\":\""); putjs((unsigned char *)q->extra + q->blen, q->ulen); PUTS("\",\"like_count\":0}}"); finish();
      break;
    case Q_LIKE:
      if (r->st == ST_OK) {
        begin_resp(r->n ? 201 : 200, close);
        PUTS("{\"liked\":true,\"already_liked\":"); if (r->n) PUTS("false"); else PUTS("true");
        PUTS(",\"post_id\":"); putu(q->id); PUTS("}"); finish();
      } else if (r->st == ST_ERR && !strcmp(r->code, "23503")) err(404, "post not found", close);
      else err(500, "internal server error", close);
      break;
    case Q_HEALTH:
      if (r->st == ST_OK) { begin_resp(200, close); PUTS("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":"); putu(now_s - started); PUTS("}"); finish(); }
      else {
        const char *m = r->msg ? r->msg : "pipeline aborted";
        begin_resp(503, close); PUTS("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\""); putjs((const unsigned char *)m, strlen(m)); PUTS("\"}"); finish();
      }
      break;
  }
  free(q->extra);
  c->busy = 0;
  send_resp(t, fd, close);
  if (c->open && !c->out && !c->closing && c->inlen) process(t, fd, c->in, c->inlen);
}

// The statement at the head of the ring is over (CommandComplete, ErrorResponse, or skipped after one): a write
// is held for its group's Sync, a read answered now.
static void stmt_done(th_t *t, pg_t *p, const res_t *r, const char *rows, uint32_t rn) {
  q_t q = qpop(p);
  if (q.kind == Q_INS || q.kind == Q_LIKE) {
    if (p->nheld == GROUP) { res_t lost = {.st = ST_LOST}; respond(t, &q, &lost, NULL, 0); pg_fail(t, p, "group overflow"); return; }  // cannot happen: a group is closed at GROUP writes
    if (r->st != ST_OK) p->gfail = 1;  // this one failed, or was skipped after a failure
    p->held[p->nheld] = q; p->hres[p->nheld++] = *r;
    return;
  }
  respond(t, &q, r, rows, rn);
}

// Reads every reply that has arrived. Statements and Syncs come back in the order they were sent.
static void pg_read(th_t *t, pg_t *p) {
  int eof = 0;
  for (;;) {
    if (p->icap - p->ilen < 16384) { p->icap *= 2; p->ib = realloc(p->ib, p->icap); }
    ssize_t r = read(p->sock, p->ib + p->ilen, p->icap - p->ilen);
    if (r < 0) { if (errno == EAGAIN) break; eof = 1; break; }
    if (r == 0) { eof = 1; break; }
    p->ilen += r; p->last_rx = now_ns; p->hedged = 0;
    if (p->ilen < p->icap) break;
  }
  while (p->sock >= 0) {
    const char *m = p->ib + p->scan; uint32_t avail = p->ilen - p->scan;
    if (avail < 5) break;
    uint32_t len = be32(m + 1);
    if (len < 4 || len > (1u << 28)) { pg_fail(t, p, "bad message length"); return; }
    if (avail < 1 + len) break;
    p->scan += 1 + len;
    char ty = m[0]; const char *pl = m + 5; uint32_t pn = len - 4;
    if (ty == 'N' || ty == 'S' || ty == 'A') { if (!p->n) p->done = p->scan; continue; }  // notices, parameter changes, notifications: anywhere
    if (!p->n) { pg_fail(t, p, ty == 'E' ? emsg(pl, pn) : "unexpected message"); return; }
    q_t *q = &p->q[p->head];
    if (q->kind == Q_SYNC) {
      if (ty == 'E') { p->cfail = 1; continue; }  // the commit failed
      if (ty != 'Z') { pg_fail(t, p, "unexpected message before ReadyForQuery"); return; }
      qpop(p); p->done = p->scan;
      // The group is over: committed (answer its writes), or rolled back because one write failed. Then that
      // write gets its error and the others, whose statements succeeded or were skipped, run again alone.
      int nh = p->nheld, retry = p->gfail && !p->cfail && nh > 1;
      p->nheld = p->gfail = 0;
      for (int k = 0; k < nh; k++) {
        q_t h = p->held[k]; res_t hr = p->hres[k];
        if (p->cfail) hr.st = ST_LOST;
        if (retry && hr.st != ST_ERR) resend(t, p, &h); else respond(t, &h, &hr, NULL, 0);
      }
      p->cfail = 0;
      continue;
    }
    switch (ty) {
      case '2': continue;  // BindComplete
      case 'D':
        p->cur.n++;
        if (q->kind == Q_INS) {  // RETURNING id, created_at
          const char *v[2]; int32_t l[2];
          if (p->cur.n == 1 && row_cols(pl, pn, 2, v, l) && l[0] == 8 && l[1] == 8) { p->cur.id = be64(v[0]); p->cur.ts = be64(v[1]); }
          else p->cur.n = 0;  // (a second row leaves n at 0 too, so the insert answers 500)
        }
        continue;
      case 'C': {  // CommandComplete: the statement is done; its rows are the messages since the last one
        res_t r = p->cur; r.st = ST_OK;
        if (q->kind == Q_LIKE) r.n = pn >= 11 && !memcmp(pl, "INSERT 0 1", 11);
        const char *rows = p->ib + p->done; uint32_t rn = m - rows;
        memset(&p->cur, 0, sizeof p->cur); p->done = p->scan;
        stmt_done(t, p, &r, rows, rn);
        continue;
      }
      case 'E': {  // this statement failed; Postgres skips the rest of the group up to its Sync
        res_t r = {.st = ST_ERR, .msg = emsg(pl, pn)}; const char *code = efield(pl, pn, 'C');
        if (code) snprintf(r.code, sizeof r.code, "%s", code);
        memset(&p->cur, 0, sizeof p->cur); p->done = p->scan;
        stmt_done(t, p, &r, NULL, 0);
        res_t ab = {.st = ST_ABORT};
        while (p->sock >= 0 && p->n && p->q[p->head].kind != Q_SYNC) stmt_done(t, p, &ab, NULL, 0);
        continue;
      }
      default: pg_fail(t, p, "unexpected message"); return;
    }
  }
  if (p->sock < 0) return;
  if (eof) { pg_fail(t, p, "connection closed"); return; }
  if (p->done) { memmove(p->ib, p->ib + p->done, p->ilen - p->done); p->ilen -= p->done; p->scan -= p->done; p->done = 0; }
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
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);  // inherited by accepted sockets, as is the next one
  // Retransmission timeouts stay linear (one RTO each) instead of doubling while few packets are in flight,
  // which is always the case here: a response lost twice costs 400 ms instead of 600, three times 600 instead of 1,400.
  setsockopt(fd, IPPROTO_TCP, TCP_THIN_LINEAR_TIMEOUTS, &one, sizeof one);
  // A connection is handed to accept() only once its first request has arrived, so accept_conns can serve it
  // in the same pass instead of waking up a second time for EPOLLIN.
  setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &one, sizeof one);
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
    // Delayed-ACK mode from the start: after a user's 10+ s of think time the kernel would otherwise ACK each
    // request at once (a gap longer than the RTO puts it in quick-ACK mode), a packet the response carries anyway.
    int zero = 0; setsockopt(c, IPPROTO_TCP, TCP_QUICKACK, &zero, sizeof zero);
    conns[c].open = 1;
    ep_set(t->ep, EPOLL_CTL_ADD, c, c, EPOLLIN);
    on_readable(t, c);  // TCP_DEFER_ACCEPT: the request is usually already here; EAGAIN if not
  }
}

static th_t *g_ts;
static void *run(void *arg) {
  th_t *t = arg;
#ifndef NO_IDLE
  // The loops run under SCHED_IDLE, the lowest scheduling class, so on the same core anything else wins: above
  // all ksoftirqd, which does the receive work once the kernel defers it there under load. The box's NIC has one
  // queue, so if receive work waits behind a busy loop the NIC's ring overflows and the packets are lost (each
  // one a 200 ms retransmit). Only these threads' own policy; no affinity, nothing box-wide.
  struct sched_param sp = {0};
  if (pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp)) fprintf(stderr, "SCHED_IDLE not set\n");
#endif
  arena = malloc(OBUF); zbuf = malloc(OBUF);
  now_s = time(NULL);
  for (int i = 0; i < t->npg; i++) pg_connect(t, i);
  for (int i = 0; i < g_nports; i++) t->lfd[t->nl++] = listen_on(g_ports[i]);
  listeners(t, EPOLL_CTL_ADD);
  struct epoll_event evs[NEV]; struct timespec t0, t1; time_t report_at = 0; unsigned long reported = 0;
  for (;;) {
    int n = epoll_wait(t->ep, evs, NEV, 50);  // wakes at least every 50 ms, so hedge() sees a stall with no events
    if (n < 0) { if (errno == EINTR) continue; perror("epoll_wait"); exit(1); }
    clock_gettime(CLOCK_MONOTONIC, &t0); now_s = time(NULL); now_ns = t0.tv_sec * 1000000000LL + t0.tv_nsec;
    if (t == g_ts && now_s >= report_at) {
      unsigned long h = __atomic_load_n(&hedged_total, __ATOMIC_RELAXED);
      if (h != reported) fprintf(stderr, "reads sent again after a stalled Postgres connection: %lu so far\n", h);
      reported = h; report_at = now_s + 10;
    }
    for (int i = 0; i < n; i++) {
      uint64_t tag = evs[i].data.u64; uint32_t e = evs[i].events; int id = (int)tag;
      if (tag >= TAG_PG) {
        pg_t *p = &t->pg[id];
        if (p->sock < 0) continue;
        if (e & (EPOLLIN | EPOLLHUP | EPOLLERR)) pg_read(t, p);
        if (p->sock >= 0 && (e & EPOLLOUT)) p->dirty = 1;
      } else if (tag >= TAG_LISTEN) accept_conns(t, t->lfd[id]);
      else if (conns[id].open) {
        if (e & EPOLLOUT) on_writable(t, id);
        else if (e & (EPOLLIN | EPOLLHUP | EPOLLERR)) on_readable(t, id);
      }
    }
#ifndef NO_HEDGE
    hedge(t);
#endif
    for (int i = 0; i < t->npg; i++) {  // one Sync for this iteration's batch on each connection, then send everything
      pg_t *p = &t->pg[i];
      if (p->sock < 0 || !p->dirty) continue;
      if (p->unsynced) pg_sync(p);
      p->dirty = 0; pg_flush(t, p);
    }
    if (t->paused_until && now_s >= t->paused_until) { t->paused_until = 0; listeners(t, EPOLL_CTL_ADD); }
    // Fixed cadence: a pass that did not fill evs lasts BATCH_NS from its start, so events pile up for the next
    // one and it serves more per wake-up, per Sync and per commit. A full pass goes straight on.
    if (n && n < NEV) {
      clock_gettime(CLOCK_MONOTONIC, &t1);
      long left = BATCH_NS - ((t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec));
      if (left > 0) { struct timespec ts = {0, left}; nanosleep(&ts, NULL); }
    }
  }
  return NULL;
}

int main(void) {
  const char *secret = getenv("JWT_SECRET"), *port = getenv("PORT"), *extra = getenv("EXTRA_PORT");
  const char *dburl = getenv("DATABASE_URL");
  if (!dburl || !secret) { fprintf(stderr, "DATABASE_URL and JWT_SECRET are required\n"); return 1; }
  if (!parse_url(dburl)) { fprintf(stderr, "DATABASE_URL must be postgres://user:pass@host:port/db\n"); return 1; }
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
  th_t *ts = g_ts = calloc(nthreads, sizeof *ts);
  for (int i = 0; i < nthreads; i++) {
    ts[i].ep = epoll_create1(EPOLL_CLOEXEC); ts[i].npg = npg_per; ts[i].pg = calloc(npg_per, sizeof(pg_t));
    for (int k = 0; k < npg_per; k++) ts[i].pg[k].sock = -1;
    if (i) { pthread_t th; pthread_create(&th, NULL, run, &ts[i]); }
  }
  run(&ts[0]);
}
