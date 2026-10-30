// ============================================================================
// Runtime Bridge for Der Plankalkul (Konrad Zuse, 1942-1945)
// Million Users Challenge — plankalkul-zuse1942-Paolozen79
//
// Links the C functions emitted by `plankalkul --emit-c` from:
//   - src/zuse_1942.pk2d (Zuse's 1942 2D V/K/S 4-row grid notation)
//   - src/feed_plans.pk  (Linearised Plankalkul: Wahrheitstafeln + P Delta 1..7)
// with Linux epoll (one share-nothing loop per CPU core, SO_REUSEPORT),
// non-blocking libpq 18.6 in pipeline mode, and Intel ISA-L 2.31.1 igzip.
// ============================================================================
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#ifdef __SSE2__
#include <emmintrin.h>
#endif
#include "igzip_lib.h"
#include "libpq-fe.h"
#include "pk_bridge.h"

// ---------- Plankalkul Compiler ABI Types & Generated Plan Declarations ----------
typedef struct { int64_t r0; int64_t r1; } pk_result2_t;
typedef struct { int64_t r0; int64_t r1; int64_t r2; } pk_result3_t;
typedef struct { int64_t r0; int64_t r1; int64_t r2; int64_t r3; } pk_result4_t;

// Plans compiled from src/zuse_1942.pk2d (1942 2D V/K/S notation)
extern pk_result3_t pk_zuse_civil_date(int64_t pk_v0);
extern pk_result4_t pk_zuse_clock_hms(int64_t pk_v0, int64_t pk_v1);
extern pk_result3_t pk_zuse_pg_us_split(int64_t pk_v0);
extern int64_t      pk_zuse_like_status(int64_t pk_v0);
extern int64_t      pk_zuse_uptime(int64_t pk_v0, int64_t pk_v1);
extern pk_result2_t pk_zuse_pool_split(int64_t pk_v0, int64_t pk_v1);

// Plans compiled from src/feed_plans.pk (Wahrheitstafel + P Delta 1..7)
extern int64_t      pk_parse_post_id(int64_t pk_v0, int64_t pk_v1);
extern int64_t      pk_js_ws_len_at(int64_t pk_v0, int64_t pk_v1, int64_t pk_v2);
extern int64_t      pk_js_ws_len_before(int64_t pk_v0, int64_t pk_v1, int64_t pk_v2);
extern pk_result3_t pk_trim_and_count_body(int64_t pk_v0, int64_t pk_v1);
extern pk_result2_t pk_verify_jwt_claims(int64_t pk_v0, int64_t pk_v1, int64_t pk_v2,
                                         int64_t pk_v3, int64_t pk_v4, int64_t pk_v5, int64_t pk_v6);
extern pk_result3_t pk_route_http_request(int64_t pk_v0, int64_t pk_v1, int64_t pk_v2,
                                          int64_t pk_v3, int64_t pk_v4);
extern int64_t      pk_batch_pause_ns(int64_t pk_v0, int64_t pk_v1, int64_t pk_v2);

#define RBUF (64 * 1024)
#define MAXREQ (64 * 1024)
#define OBUF (256 * 1024)
#define GZMIN 1024
#define BATCH_MIN 24
#define BATCH_NS 0

// 40-byte connection slot per file descriptor (~40 MB RSS at 1,000,000 open connections).
// Idle connections own zero buffers.
typedef struct {
  char *in; uint32_t inlen, incap;
  char *out; uint32_t outlen, outoff;
  uint32_t gen;
  uint8_t open, busy, closing;
} conn_t;
static conn_t *conns;
static long maxfd;

static __thread char *arena;
static __thread size_t alen;
static __thread time_t now_s;
static time_t started;

// Bounds-checked append: an oversized response sets `ovf` and finish() turns it into a 500.
static __thread int ovf;
static inline void put(const void *p, size_t n) {
  if (__builtin_expect(alen + n > OBUF, 0)) { ovf = 1; return; }
  memcpy(arena + alen, p, n); alen += n;
}
#define PUTS(s) put(s, sizeof(s) - 1)
static const char DEC2[200] =
  "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
  "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
  "8081828384858687888990919293949596979899";
static inline void putu(uint64_t v) {
  char b[24]; int i = 24;
  while (v >= 100) { i -= 2; memcpy(b + i, DEC2 + (v % 100) * 2, 2); v /= 100; }
  if (v >= 10) { i -= 2; memcpy(b + i, DEC2 + v * 2, 2); }
  else b[--i] = '0' + v;
  put(b + i, 24 - i);
}
static const char hexd[] = "0123456789abcdef";
static void putjs(const unsigned char *s, size_t n) {
  size_t i = 0, run = 0;
#ifdef __SSE2__
  const __m128i v1f = _mm_set1_epi8(0x1f), vq = _mm_set1_epi8('"'), vbs = _mm_set1_epi8('\\');
#endif
  while (i < n) {
#ifdef __SSE2__
    while (i + 16 <= n) {
      __m128i v = _mm_loadu_si128((const __m128i *)(s + i));
      __m128i m = _mm_or_si128(_mm_cmpeq_epi8(_mm_max_epu8(v, v1f), v1f),
                  _mm_or_si128(_mm_cmpeq_epi8(v, vq), _mm_cmpeq_epi8(v, vbs)));
      int mask = _mm_movemask_epi8(m);
      if (!mask) { i += 16; continue; }
      i += __builtin_ctz(mask);
      break;
    }
    if (i >= n) break;
#endif
    unsigned char c = s[i];
    if (c >= 0x20 && c != '"' && c != '\\') { i++; continue; }
    put(s + run, i - run); run = ++i;
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

// Format binary PostgreSQL TIMESTAMPTZ (microseconds since 2000-01-01 UTC)
// using our three 1942 2D Plankalkul Plans:
//   - pk_zuse_pg_us_split (Plan 3 in src/zuse_1942.pk2d)
//   - pk_zuse_civil_date  (Plan 1 in src/zuse_1942.pk2d)
//   - pk_zuse_clock_hms   (Plan 2 in src/zuse_1942.pk2d)
static void put_ts(int64_t us) {
  if (__builtin_expect(alen + 24 > OBUF, 0)) { ovf = 1; return; }
  pk_result3_t sp = pk_zuse_pg_us_split(us);
  int64_t days = sp.r0, sod = sp.r1, r = sp.r2;
  if (r < 0) { r += 1000000; sod--; }
  if (sod < 0) { sod += 86400; days--; }
  pk_result3_t ymd = pk_zuse_civil_date(days);
  pk_result4_t hms = pk_zuse_clock_hms(sod, r);
  char *d = arena + alen;
  unsigned y = (unsigned)ymd.r0, ms = (unsigned)hms.r3;
  memcpy(d,      DEC2 + (y / 100) * 2, 2);
  memcpy(d + 2,  DEC2 + (y % 100) * 2, 2); d[4] = '-';
  memcpy(d + 5,  DEC2 + (unsigned)ymd.r1 * 2, 2); d[7] = '-';
  memcpy(d + 8,  DEC2 + (unsigned)ymd.r2 * 2, 2); d[10] = 'T';
  memcpy(d + 11, DEC2 + (unsigned)hms.r0 * 2, 2); d[13] = ':';
  memcpy(d + 14, DEC2 + (unsigned)hms.r1 * 2, 2); d[16] = ':';
  memcpy(d + 17, DEC2 + (unsigned)hms.r2 * 2, 2); d[19] = '.';
  d[20] = '0' + ms / 100;
  memcpy(d + 21, DEC2 + (ms % 100) * 2, 2); d[23] = 'Z';
  alen += 24;
}

static __thread size_t body_start, cl_pos;
static __thread int want_gz;
static __thread char *zbuf;
static __thread size_t zlen;
// Huffman tables are built ONCE at startup from a synthetic, hard-coded /feed-shaped JSON
// sample (no database data, no response bytes), so nothing derived from served data is kept.
static struct isal_hufftables gz_huff;
static void gz_init_tables(void) {
  static const char row[] =
    "{\"id\":123456,\"body\":\"the quick brown fox jumps over the lazy dog, then writes another post "
    "about it: hello world, this is my feed and these are my thoughts for today.\","
    "\"created_at\":\"2025-06-14T09:41:27.385Z\",\"author\":\"user_48213\",\"like_count\":4},";
  char sample[sizeof row * 20 + 32]; size_t n = 0;
  memcpy(sample, "{\"posts\":[", 10); n = 10;
  for (int i = 0; i < 20; i++) { memcpy(sample + n, row, sizeof row - 1); n += sizeof row - 1; }
  sample[n - 1] = ']'; sample[n++] = '}';
  struct isal_huff_histogram hist;
  memset(&hist, 0, sizeof hist);
  isal_update_histogram((uint8_t *)sample, (int)n, &hist);
  isal_create_hufftables(&gz_huff, &hist);
}

static size_t gzip_compress(const char *in, size_t n, char *out, size_t cap) {
  struct isal_zstream s;
  isal_deflate_stateless_init(&s);
  s.next_in = (uint8_t *)in;
  s.avail_in = n;
  s.next_out = (uint8_t *)out;
  s.avail_out = cap;
  s.gzip_flag = IGZIP_GZIP;
  s.level = 0;
  s.hufftables = &gz_huff;
  return isal_deflate_stateless(&s) == COMP_OK ? cap - s.avail_out : 0;
}

static __thread int resp_close;
static void begin_resp(int status, int close) {
  alen = 0; zlen = 0; ovf = 0; resp_close = close;
  switch (status) {
    case 200: PUTS("HTTP/1.1 200 OK\r\n"); break;
    case 201: PUTS("HTTP/1.1 201 Created\r\n"); break;
    case 400: PUTS("HTTP/1.1 400 Bad Request\r\n"); break;
    case 401: PUTS("HTTP/1.1 401 Unauthorized\r\n"); break;
    case 404: PUTS("HTTP/1.1 404 Not Found\r\n"); break;
    case 503: PUTS("HTTP/1.1 503 Service Unavailable\r\n"); break;
    default:  PUTS("HTTP/1.1 500 Internal Server Error\r\n"); break;
  }
  if (close) PUTS("Connection: close\r\n");
  PUTS("Content-Type: application/json\r\nContent-Length:       \r\n\r\n");
  cl_pos = alen - 10; body_start = alen;
}

static void err(int status, const char *msg, int close);
static void finish(void) {
  if (__builtin_expect(ovf, 0)) { ovf = 0; want_gz = 0; err(500, "internal server error", resp_close); return; }
  size_t n = alen - body_start;
  char *p = arena + cl_pos + 6;
  if (want_gz && n >= GZMIN && (zlen = gzip_compress(arena + body_start, n, zbuf, OBUF))) {
    alen = cl_pos - 16;
    PUTS("Content-Encoding: gzip\r\nContent-Length: "); putu(zlen); PUTS("\r\n\r\n");
    return;
  }
  do { *--p = '0' + n % 10; n /= 10; } while (n);
}

static void err(int status, const char *msg, int close) {
  begin_resp(status, close);
  PUTS("{\"error\":\""); put(msg, strlen(msg)); PUTS("\"}");
  finish();
}

// ---------- SHA-256 / HMAC (precomputed ipad/opad key schedule) ----------
typedef struct { uint32_t h[8]; uint64_t n; unsigned char b[64]; uint32_t bl; } sha_t;
static const uint32_t K[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(uint32_t *h, const unsigned char *p) {
  uint32_t w[64], a, b, c, d, e, f, g, k, t1, t2;
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | p[4*i+1] << 16 | p[4*i+2] << 8 | p[4*i+3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
    uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
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
  if (s->bl) {
    size_t t = 64 - s->bl; if (t > n) t = n;
    memcpy(s->b + s->bl, p, t); s->bl += t; p += t; n -= t;
    if (s->bl == 64) { sha_block(s->h, s->b); s->bl = 0; }
  }
  for (; n >= 64; p += 64, n -= 64) sha_block(s->h, p);
  if (n) { memcpy(s->b, p, n); s->bl = n; }
}
static void sha_fin(sha_t *s, unsigned char *out) {
  uint64_t bits = s->n * 8; unsigned char pad = 0x80, z[64] = {0}, len[8];
  sha_upd(s, &pad, 1);
  sha_upd(s, z, (s->bl <= 56 ? 56 : 120) - s->bl);
  for (int i = 0; i < 8; i++) len[i] = bits >> (56 - 8 * i);
  sha_upd(s, len, 8);
  for (int i = 0; i < 8; i++) {
    out[4*i] = s->h[i] >> 24; out[4*i+1] = s->h[i] >> 16;
    out[4*i+2] = s->h[i] >> 8; out[4*i+3] = s->h[i];
  }
}
static sha_t hm_in, hm_out;
static void hmac_key(const char *key) {
  unsigned char k[64] = {0}, pad[64]; size_t kl = strlen(key);
  if (kl > 64) { sha_t s; sha_init(&s); sha_upd(&s, (const unsigned char *)key, kl); sha_fin(&s, k); }
  else memcpy(k, key, kl);
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
static int b64dec(const char *s, size_t n, unsigned char *out) {
  if (n % 4 == 1) return -1;
  uint32_t acc = 0; int bits = 0, o = 0;
  for (size_t i = 0; i < n; i++) {
    int v = b64t[(unsigned char)s[i]]; if (v < 0) return -1;
    acc = acc << 6 | v; bits += 6;
    if (bits >= 8) { bits -= 8; out[o++] = acc >> bits; }
  }
  return o;
}

// ---------- JSON (strict RFC 8259) ----------
typedef struct { const char *p, *e; } js_t;
static inline void ws(js_t *j) { while (j->p < j->e && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++; }
static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int utf8_len(const unsigned char *p, const unsigned char *e) {
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
static long jstr(js_t *j, char *out, size_t cap) {
  const unsigned char *p = (const unsigned char *)j->p + 1, *e = (const unsigned char *)j->e;
  size_t o = 0; int over = 0;
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
          if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;
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
static int jvalue(js_t *j, int depth) {
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

// ---------- JWT Auth (delegates claim & sub validation to P Delta 5 in Plankalkul) ----------
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
  if (!jdoc((char *)pl, pn, pw, 4)) return bad;

  int64_t has_exp = pw[0].v != NULL ? 1 : 0;
  int64_t exp_ok = 1, nbf_ok = 1;
  for (int i = 0; i < 2; i++) if (pw[i].v) {
    if (*pw[i].v != '-' && (*pw[i].v < '0' || *pw[i].v > '9')) return bad;
    char nb[64]; size_t l = pw[i].ve - pw[i].v; if (l >= sizeof nb) return bad;
    memcpy(nb, pw[i].v, l); nb[l] = 0; double v = strtod(nb, NULL);
    if (i == 0) exp_ok = ((double)now_s < v) ? 1 : 0;
    else        nbf_ok = ((double)now_s >= v) ? 1 : 0;
  }

  int64_t has_sub = (pw[2].v && *pw[2].v == '"') ? 1 : 0;
  char sb[32]; long sl = -1;
  if (has_sub) {
    js_t sj = {pw[2].v, pw[2].ve};
    sl = jstr(&sj, sb, sizeof sb);
    if (sl < 0) has_sub = 0;
  }

  int64_t has_un = (pw[3].v && *pw[3].v == '"') ? 1 : 0;
  long ul = -1;
  if (has_un) {
    js_t uj = {pw[3].v, pw[3].ve};
    ul = jstr(&uj, uname, 1024);
    if (ul < 0) has_un = 0;
  }

  // Call P Delta 5 (verify_jwt_claims) in Plankalkul
  pk_result2_t vr = pk_verify_jwt_claims(has_exp, exp_ok, nbf_ok, has_sub,
                                         (int64_t)(uintptr_t)sb, sl > 0 ? sl : 0, has_un);
  if (vr.r0 == 1) return bad;
  if (vr.r0 != 0) return payload;
  *uid = vr.r1;
  *unl = (size_t)ul;
  return NULL;
}

// ---------- Postgres Pipeline Engine ----------
enum { Q_FEED = 0, Q_POST = 1, Q_INS = 2, Q_LIKE = 3, Q_HEALTH = 4, Q_SYNC = 5 };
typedef struct { int fd; uint32_t gen; uint8_t kind, close, gz, tries; int64_t id; char *extra; uint32_t blen, ulen; } q_t;
typedef struct {
  PGconn *c; int sock;
  q_t *q; uint32_t head, n, cap;
  uint8_t got, unsynced, dirty, wout; uint16_t inbatch;
  q_t held; PGresult *hres; uint8_t has_held, held_fail;
  // Background reconnect: a helper thread runs the blocking connect + PREPAREs and
  // hands the ready PGconn back through `pending` (rc_state: 0 idle, 1 connecting, 2 done).
  PGconn *pending; uint8_t rc_state;
} pg_t;
typedef struct { int ep, lfd[2], nl, npg, nwrite; pg_t *pg; time_t retry_at, paused_until; } th_t;

static const char *dburl;
static int nthreads, npg_per, nwrite_per;
static const char *g_host;
static int g_ports[2], g_nports;
static const char *cols = "SELECT p.id, p.body, p.created_at, u.username, "
  "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id";

#define TAG_LISTEN (1ULL << 32)
#define TAG_PG (2ULL << 32)
static void ep_set(int ep, int op, int fd, uint64_t tag, uint32_t ev) {
  struct epoll_event e = {.events = ev, .data.u64 = tag};
  epoll_ctl(ep, op, fd, &e);
}

// Blocking connect + PREPARE; returns a non-blocking pipeline-mode PGconn or NULL.
static PGconn *pg_open(void) {
  static const char *const kw[] = {"dbname", "connect_timeout", NULL};
  const char *const val[] = {dburl, "3", NULL};
  PGconn *c = PQconnectdbParams(kw, val, 1);
  if (PQstatus(c) != CONNECTION_OK) { fprintf(stderr, "connect: %s", PQerrorMessage(c)); PQfinish(c); return NULL; }
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
    if (!ok) { fprintf(stderr, "prepare: %s", PQerrorMessage(c)); PQfinish(c); return NULL; }
  }
  if (!PQenterPipelineMode(c) || PQsetnonblocking(c, 1)) { PQfinish(c); return NULL; }
  int one = 1;
  setsockopt(PQsocket(c), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  return c;
}

static void pg_attach(th_t *t, int i, PGconn *c) {
  pg_t *p = &t->pg[i];
  p->c = c; p->sock = PQsocket(c);
  p->got = p->unsynced = p->dirty = p->wout = 0; p->inbatch = 0;
  ep_set(t->ep, EPOLL_CTL_ADD, p->sock, TAG_PG | i, EPOLLIN);
}

static void *pg_reconnect_thread(void *arg) {
  pg_t *p = arg;
  p->pending = pg_open();
  __atomic_store_n(&p->rc_state, 2, __ATOMIC_RELEASE);
  return NULL;
}

// Never blocks the event loop: spawns a detached helper thread for the connect.
static void pg_reconnect_async(pg_t *p) {
  p->rc_state = 1; p->pending = NULL;
  pthread_attr_t a; pthread_t th;
  pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&th, &a, pg_reconnect_thread, p)) p->rc_state = 0;
  pthread_attr_destroy(&a);
}

// Called once per event-loop turn: adopt connections finished by helper threads.
static void pg_adopt(th_t *t) {
  for (int i = 0; i < t->npg; i++) {
    pg_t *p = &t->pg[i];
    if (__atomic_load_n(&p->rc_state, __ATOMIC_ACQUIRE) != 2) continue;
    p->rc_state = 0;
    if (p->pending) { pg_attach(t, i, p->pending); p->pending = NULL; }
  }
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
static void pg_fail(th_t *t, pg_t *p) {
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

#define PACK 16
static void pg_flush(th_t *t, pg_t *p);
static pg_t *pick(th_t *t, int write) {
  pg_t *b = NULL, *d = NULL; int nw = t->nwrite;
  if (t->retry_at != now_s) {
    t->retry_at = now_s;
    for (int i = 0; i < t->npg; i++) if (!t->pg[i].c && !t->pg[i].rc_state) pg_reconnect_async(&t->pg[i]);
  }
  for (int i = 0; i < t->npg; i++) {
    pg_t *p = &t->pg[i];
    if (!p->c || (i >= t->npg - nw) != write) continue;
    if (!b || p->n < b->n) b = p;
    if (!write && p->dirty && p->inbatch < PACK && (!d || p->n < d->n)) d = p;
  }
  return (d && b && d->n <= b->n + PACK) ? d : b;
}

static void pg_sync(pg_t *p) {
  if (!PQsendPipelineSync(p->c)) return;
  q_t *q = qpush(p); memset(q, 0, sizeof *q); q->kind = Q_SYNC; p->unsynced = 0;
}

static int send_q(pg_t *p, int kind, int64_t id, int64_t uid, const char *s, size_t sl) {
  uint64_t a = __builtin_bswap64(uid), b = __builtin_bswap64(id);
  const char *v[2]; int len[2] = {8, 8}, fmt[2] = {1, 1};
  switch (kind) {
    case Q_FEED:   return PQsendQueryPrepared(p->c, "f", 0, NULL, NULL, NULL, 1);
    case Q_HEALTH: return PQsendQueryPrepared(p->c, "h", 0, NULL, NULL, NULL, 1);
    case Q_POST:   v[0] = (char *)&b; return PQsendQueryPrepared(p->c, "p", 1, v, len, fmt, 1);
    case Q_LIKE:   v[0] = (char *)&a; v[1] = (char *)&b; return PQsendQueryPrepared(p->c, "l", 2, v, len, fmt, 1);
    default:       v[0] = (char *)&a; v[1] = s; len[1] = sl; return PQsendQueryPrepared(p->c, "i", 2, v, len, fmt, 1);
  }
}

static int enqueue(th_t *t, int fd, int kind, int close, int64_t id, int64_t uid, const char *s, size_t sl, const char *un, size_t unl) {
  int is_write = (kind == Q_INS || kind == Q_LIKE);
  pg_t *p = pick(t, is_write);
  if (!p) return 0;
  if (!send_q(p, kind, id, uid, s, sl)) return 0;
  q_t *q = qpush(p);
  *q = (q_t){fd, __atomic_load_n(&conns[fd].gen, __ATOMIC_RELAXED), kind, close, want_gz, 0, id, NULL, 0, 0};
  if (kind == Q_INS) {
    q->extra = malloc(sl + unl + 1); memcpy(q->extra, s, sl); memcpy(q->extra + sl, un, unl);
    q->blen = sl; q->ulen = unl;
  }
  p->dirty = 1; p->unsynced = 1; p->inbatch++;
  if (is_write) pg_sync(p);
  else if (p->inbatch >= PACK) { pg_sync(p); p->dirty = 0; p->inbatch = 0; pg_flush(t, p); }
  return 1;
}

// A read query that was skipped because an earlier query in the same pipeline batch
// failed (PGRES_PIPELINE_ABORTED) is re-sent once, so one bad query can't 500 a whole batch.
static int resend_read(th_t *t, const q_t *src) {
  pg_t *p = pick(t, 0);
  if (!p || !send_q(p, src->kind, src->id, 0, NULL, 0)) return 0;
  q_t *q = qpush(p); *q = *src; q->tries++;
  p->dirty = 1; p->unsynced = 1; p->inbatch++;
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

// ---------- Client Connections ----------
static void conn_close(th_t *t, int fd) {
  conn_t *c = &conns[fd];
  free(c->in); free(c->out);
  uint32_t g = c->gen + 1; memset(c, 0, sizeof *c); __atomic_store_n(&c->gen, g, __ATOMIC_RELAXED);
  close(fd);
}
static void process(th_t *t, int fd, char *buf, size_t n);
static void send_resp(th_t *t, int fd, int close) {
  conn_t *c = &conns[fd];
  size_t total = alen + zlen;
  ssize_t w;
  if (!zlen) {
    w = write(fd, arena, alen);
  } else {
    struct iovec iv[2] = {{arena, alen}, {zbuf, zlen}};
    w = writev(fd, iv, 2);
  }
  if (w < 0) { if (errno != EAGAIN) { conn_close(t, fd); return; } w = 0; }
  if (close) c->closing = 1;
  if ((size_t)w < total) {
    c->outlen = total - w; c->outoff = 0; c->out = malloc(c->outlen);
    if ((size_t)w < alen) { memcpy(c->out, arena + w, alen - w); memcpy(c->out + alen - w, zbuf, zlen); }
    else memcpy(c->out, zbuf + (w - alen), c->outlen);
    ep_set(t->ep, EPOLL_CTL_MOD, fd, fd, EPOLLOUT);
  } else if (close) conn_close(t, fd);
}

static void respond(th_t *t, q_t *q, PGresult *r, int fail) {
  int fd = q->fd; conn_t *c = &conns[fd];
  if (q->gen != __atomic_load_n(&c->gen, __ATOMIC_RELAXED) || !c->open) { free(q->extra); return; }
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
        // Call 2D Plankalkul Plan 4 (zuse_like_status)
        int status = (int)pk_zuse_like_status(created ? 1 : 0);
        begin_resp(status, close);
        PUTS("{\"liked\":true,\"already_liked\":"); if (created) PUTS("false"); else PUTS("true");
        PUTS(",\"post_id\":"); putu(q->id); PUTS("}"); finish();
      } else {
        const char *st = r ? PQresultErrorField(r, PG_DIAG_SQLSTATE) : NULL;
        if (st && !strcmp(st, "23503")) err(404, "post not found", close); else err(500, "internal server error", close);
      }
      break;
    case Q_HEALTH:
      if (s == PGRES_TUPLES_OK) {
        // Call 2D Plankalkul Plan 5 (zuse_uptime)
        int64_t up = pk_zuse_uptime(now_s, started);
        begin_resp(200, close); PUTS("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":"); putu(up); PUTS("}"); finish();
      } else {
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

static void pg_read(th_t *t, pg_t *p) {
  while (p->c) {
    if (!PQconsumeInput(p->c)) { pg_fail(t, p); return; }
    int progressed = 0;
    while (p->c && p->n && !PQisBusy(p->c)) {
      progressed = 1;
      q_t *q = &p->q[p->head];
      PGresult *r = PQgetResult(p->c);
      if (q->kind == Q_SYNC) {
        if (!r) break;
        if (PQresultStatus(r) != PGRES_PIPELINE_SYNC) { p->held_fail = 1; PQclear(r); continue; }
        PQclear(r);
        p->head = (p->head + 1) & (p->cap - 1); p->n--;
        if (p->has_held) {
          q_t h = p->held; PGresult *hr = p->hres; int f = p->held_fail;
          p->has_held = p->held_fail = 0;
          respond(t, &h, hr, f); PQclear(hr);
        }
        continue;
      }
      if (p->got) {
        if (r) { PQclear(r); continue; }
        p->got = 0; p->head = (p->head + 1) & (p->cap - 1); p->n--;
        continue;
      }
      p->got = 1;
      q_t cq = *q;
      if (cq.kind == Q_INS || cq.kind == Q_LIKE) { p->held = cq; p->hres = r; p->has_held = 1; p->held_fail = 0; continue; }
      if (r && !cq.tries && PQresultStatus(r) == PGRES_PIPELINE_ABORTED && resend_read(t, &cq)) { PQclear(r); continue; }
      respond(t, &cq, r, r == NULL); PQclear(r);
    }
    if (!progressed || !p->n) break;
  }
}

// ---------- Request Routing & Validation via Plankalkul Plans ----------
static void handle(th_t *t, int fd, const char *m, size_t ml, const char *path, size_t pl,
                   const char *authh, size_t al, const char *body, size_t bl, int close) {
  const char *q = memchr(path, '?', pl); if (q) pl = q - path;
  int64_t m_code = (ml == 3 && !memcmp(m, "GET", 3)) ? 1 : ((ml == 4 && !memcmp(m, "POST", 4)) ? 2 : 0);
  int64_t path_kind = 0;
  const char *seg = NULL;
  size_t seg_len = 0;

  if (pl == 5 && !memcmp(path, "/feed", 5)) path_kind = 1;
  else if (pl == 7 && !memcmp(path, "/health", 7)) path_kind = 2;
  else if (pl == 6 && !memcmp(path, "/posts", 6)) path_kind = 3;
  else if (pl > 7 && !memcmp(path, "/posts/", 7)) {
    seg = path + 7;
    const char *sl = memchr(seg, '/', pl - 7);
    if (!sl) {
      path_kind = 4;
      seg_len = pl - 7;
    } else if ((size_t)(path + pl - sl) == 5 && !memcmp(sl, "/like", 5)) {
      path_kind = 5;
      seg_len = sl - seg;
    }
  }

  // Call P Delta 6 (route_http_request) + Wahrheitstafel (table_route_policy) in Plankalkul
  pk_result3_t rt = pk_route_http_request(m_code, path_kind, (int64_t)(uintptr_t)seg, (int64_t)seg_len, 0);

  int64_t uid = 0;
  char un[1024];
  size_t unl = 0;
  const char *s = NULL;
  size_t slen = 0;
  static __thread char txt[MAXREQ];

  if (rt.r0 == 1) {
    // Authenticated endpoint: SPEC requires checking JWT auth BEFORE post id or body!
    const char *e = auth(authh, al, &uid, un, &unl);
    if (e) { err(401, e, close); return; }

    if (rt.r1 == Q_LIKE) {
      // Now re-enter P Delta 6 with V4=1 (auth verified) to parse and validate :id via P Delta 1
      rt = pk_route_http_request(m_code, path_kind, (int64_t)(uintptr_t)seg, (int64_t)seg_len, 1);
    } else if (rt.r1 == Q_INS) {
      want_t w[1] = {{"body"}};
      if (!jdoc(body, bl, w, 1)) { err(400, "malformed JSON body", close); return; }
      if (!w[0].v || *w[0].v != '"') { err(400, "body is required", close); return; }
      js_t bj = {w[0].v, w[0].ve};
      long n = jstr(&bj, txt, sizeof txt);
      if (n < 0 || memchr(txt, 0, (size_t)n)) { err(400, "malformed JSON body", close); return; }
      // Call P Delta 4 (trim_and_count_body) in Plankalkul (uses Zuse W and W1 loops)
      pk_result3_t tb = pk_trim_and_count_body((int64_t)(uintptr_t)txt, (int64_t)n);
      if (tb.r0 == 1) { err(400, "body is required", close); return; }
      if (tb.r0 == 2) { err(400, "body must be at most 500 characters", close); return; }
      s = txt + tb.r1;
      slen = (size_t)tb.r2;
      rt.r0 = 0;
    }
  }

  if (rt.r0 == 400) { err(400, "invalid post id", close); return; }
  if (rt.r0 != 0)   { err(404, "not found", close); return; }

  int kind = (int)rt.r1;
  int64_t id = rt.r2;
  if (enqueue(t, fd, kind, close, id, uid, s, slen, un, unl)) { conns[fd].busy = 1; return; }
  if (kind != Q_HEALTH) { err(500, "internal server error", close); return; }
  begin_resp(503, close); PUTS("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"no database connection\"}"); finish();
}

static int accepts_gzip(const char *v, const char *ve) {
  const char *g = NULL;
  for (const char *s = v; s + 4 <= ve; s++) if (!strncasecmp(s, "gzip", 4)) { g = s; break; }
  if (!g) return 0;
  for (g += 4; g < ve && *g == ' '; g++) {}
  if (g + 2 < ve && *g == ';' && (g = memmem(g, ve - g, "q=", 2))) {
    char b[8] = {0}; memcpy(b, g + 2, ve - g - 2 < 7 ? ve - g - 2 : 7);
    return strtod(b, NULL) > 0;
  }
  return 1;
}
static int hdr_is(const char *p, size_t n, const char *name, size_t nl) { return n == nl && !strncasecmp(p, name, nl); }

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
        if (hdr_is(h, kn, "content-length", 14)) {
          clen = 0;
          for (char *d = v; d < ve; d++) { if (*d < '0' || *d > '9') return -1; clen = clen * 10 + *d - '0'; if (clen > MAXREQ) return -1; }
        }
        else if (hdr_is(h, kn, "authorization", 13)) { authh = v; al = ve - v; }
        else if (hdr_is(h, kn, "connection", 10)) {
          if (ve - v == 5 && !strncasecmp(v, "close", 5)) close = 1;
          else if (ve - v == 10 && !strncasecmp(v, "keep-alive", 10)) close = 0;
        }
        else if (hdr_is(h, kn, "accept-encoding", 15)) want_gz = accepts_gzip(v, ve);
        else if (hdr_is(h, kn, "expect", 6)) expect = ve - v == 12 && !strncasecmp(v, "100-continue", 12);
        else if (hdr_is(h, kn, "transfer-encoding", 17)) return -1;
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
  if (!c->open) return;
  if (used < 0) {
    // Malformed HTTP (bad request line, chunked body, oversized headers): answer 400 and close.
    if (c->out || c->busy) { conn_close(t, fd); return; }
    want_gz = 0; err(400, "bad request", 1); send_resp(t, fd, 1);
    if (!c->open) return;
    free(c->in); c->in = NULL; c->inlen = c->incap = 0;
    return;
  }
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
  if (c->inlen || c->busy) {
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
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef TCP_DEFER_ACCEPT
  setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &one, sizeof one);
#endif
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
  if (inet_pton(AF_INET, g_host, &a.sin_addr) != 1) { fprintf(stderr, "bad HOST %s\n", g_host); exit(1); }
  if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 65535)) { perror("bind/listen"); exit(1); }
  return fd;
}
static void listeners(th_t *t, int op) { for (int i = 0; i < t->nl; i++) ep_set(t->ep, op, t->lfd[i], TAG_LISTEN | i, EPOLLIN); }
static void accept_conns(th_t *t, int lfd) {
  for (int k = 0; k < 1024; k++) {
    int c = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (c < 0) {
      if (errno == EMFILE || errno == ENFILE) { listeners(t, EPOLL_CTL_DEL); t->paused_until = now_s + 1; }
      return;
    }
    if (c >= maxfd) { close(c); continue; }
    conns[c].open = 1;
    ep_set(t->ep, EPOLL_CTL_ADD, c, c, EPOLLIN);
  }
}

static void *thp_alloc(size_t sz) {
  size_t align = 2 << 20;
  size_t rsz = (sz + align - 1) & ~(align - 1);
  void *p = NULL;
  if (posix_memalign(&p, align, rsz) != 0 || !p) { p = calloc(1, sz); return p; }
#ifdef MADV_HUGEPAGE
  madvise(p, rsz, MADV_HUGEPAGE);
#endif
  memset(p, 0, sz);
  return p;
}

static void *run(void *arg) {
  th_t *t = arg;
  char *tmem = thp_alloc(2 * OBUF);
  arena = tmem; zbuf = tmem + OBUF;
  now_s = time(NULL);
  for (int i = 0; i < t->npg; i++) {
    PGconn *c = pg_open();
    if (!c) break;  // database down at startup: the rest reconnect in the background
    pg_attach(t, i, c);
  }
  for (int i = 0; i < g_nports; i++) t->lfd[t->nl++] = listen_on(g_ports[i]);
  listeners(t, EPOLL_CTL_ADD);
  struct epoll_event evs[2048];
  for (;;) {
    int n = epoll_wait(t->ep, evs, 2048, 1000);
    if (n < 0) { if (errno == EINTR) continue; perror("epoll_wait"); exit(1); }
    now_s = time(NULL);
    pg_adopt(t);
    int client_evs = 0, pg_evs = 0;
    for (int i = 0; i < n; i++) {
      uint64_t tag = evs[i].data.u64; uint32_t e = evs[i].events; int id = (int)tag;
      if (tag >= TAG_PG) {
        pg_t *p = &t->pg[id];
        if (!p->c) continue;
        pg_evs++;
        if (e & (EPOLLIN | EPOLLHUP | EPOLLERR)) pg_read(t, p);
        if (p->c && (e & EPOLLOUT)) p->dirty = 1;
      } else if (tag >= TAG_LISTEN) accept_conns(t, t->lfd[id]);
      else if (conns[id].open) {
        client_evs++;
        if (e & EPOLLOUT) on_writable(t, id);
        else if (e & (EPOLLIN | EPOLLHUP | EPOLLERR)) on_readable(t, id);
      }
    }
    for (int i = 0; i < t->npg; i++) {
      pg_t *p = &t->pg[i];
      if (!p->c || !p->dirty) continue;
      if (p->unsynced) pg_sync(p);
      p->dirty = 0; p->inbatch = 0; pg_flush(t, p);
    }
    if (t->paused_until && now_s >= t->paused_until) { t->paused_until = 0; listeners(t, EPOLL_CTL_ADD); }
    // Call P Delta 7 (batch_pause_ns) in Plankalkul only on turns with client requests and
    // no Postgres results: a turn that delivered DB results never sleeps.
    int64_t pause_ns = pk_batch_pause_ns(pg_evs ? 0 : client_evs, BATCH_MIN, BATCH_NS);
    if (pause_ns > 0) { struct timespec ts = {0, (long)pause_ns}; nanosleep(&ts, NULL); }
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
  maxfd = rl.rlim_cur > (16 << 20) ? (16 << 20) : (long)rl.rlim_cur;
  conns = thp_alloc((size_t)maxfd * sizeof *conns);
  memset(b64t, -1, sizeof b64t);
  for (int i = 0; i < 64; i++) b64t[(unsigned char)"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"[i]] = i;
  hmac_key(secret);
  gz_init_tables();
  started = time(NULL);
  cpu_set_t cs; sched_getaffinity(0, sizeof cs, &cs);
  nthreads = CPU_COUNT(&cs);
  // Call 2D Plankalkul Plan 6 (zuse_pool_split) with 128 total connections (capped at 16 per thread)
  int target_total = nthreads * 16 < 128 ? nthreads * 16 : 128;
  pk_result2_t pool = pk_zuse_pool_split(target_total, nthreads);
  npg_per = pool.r0 > 2 ? (int)pool.r0 : 2;
  nwrite_per = pool.r1 > 0 ? (int)pool.r1 : 1;
  fprintf(stderr, "[Plankalkul 1942] %d threads, %d Postgres connections each (%d write), ports %d%s\n",
          nthreads, npg_per, nwrite_per, g_ports[0], g_nports > 1 ? " + extra" : "");
  th_t *ts = calloc(nthreads, sizeof *ts);
  for (int i = 0; i < nthreads; i++) {
    ts[i].ep = epoll_create1(EPOLL_CLOEXEC);
    ts[i].npg = npg_per;
    ts[i].nwrite = nwrite_per;
    ts[i].pg = calloc(npg_per, sizeof(pg_t));
    if (i) { pthread_t th; pthread_create(&th, NULL, run, &ts[i]); }
  }
  run(&ts[0]);
  return 0;
}
