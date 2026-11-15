// HTTP/1.1 request parser and small helpers shared by the api layer.

#include "server.h"

const char* reason_of(int s) {
  switch (s) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "OK";
  }
}

// parse "positive integer" post id; 0/false=bad
bool post_id(const char* s, size_t n, int64_t* out) {
  if (UNLIKELY(n == 0 || n > 19)) return false;
  int64_t v = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned d = (uint8_t)s[i] - '0';
    if (UNLIKELY(d > 9)) return false;
    if (UNLIKELY(v > (INT64_MAX - (int64_t)d) / 10)) return false;
    v = v * 10 + (int64_t)d;
  }
  if (UNLIKELY(v <= 0)) return false;
  *out = v;
  return true;
}

// find the "\r\n\r\n" header terminator. glibc's memmem runs a non-SIMD
// Horspool loop for a 4-byte needle, so an AVX2 mask scan wins by ~5x at
// request sizes. Windows advance 29 bytes so any terminator sits wholly
// inside some 32-byte block - straddles need no carry logic.
#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
__attribute__((target("avx2"))) static const char* hterm_v(const char* s,
                                                         size_t n) {
  const __m256i CR = _mm256_set1_epi8('\r');
  const __m256i LF = _mm256_set1_epi8('\n');
  size_t i = 0;
  for (; i + 32 <= n; i += 29) {
    __m256i v = _mm256_loadu_si256((const __m256i*)(s + i));
    uint32_t cr = _mm256_movemask_epi8(_mm256_cmpeq_epi8(v, CR));
    uint32_t lf = _mm256_movemask_epi8(_mm256_cmpeq_epi8(v, LF));
    uint32_t crlf = cr & (lf >> 1);  // "\r\n" starts in-window
    uint32_t d = crlf & (crlf >> 2); // "\r\n\r\n" starts in-window
    if (UNLIKELY(d)) return s + i + __builtin_ctz(d);
  }
  for (; i + 4 <= n; i++)
    if (s[i] == '\r' && s[i + 1] == '\n' && s[i + 2] == '\r' &&
        s[i + 3] == '\n')
      return s + i;
  return nullptr;
}
static const bool g_avx2 = __builtin_cpu_supports("avx2") != 0;
#endif

static const char* hterm(const char* s, size_t n) {
#if defined(__x86_64__) && defined(__GNUC__)
  if (g_avx2) return hterm_v(s, n);
#endif
  return (const char*)memmem(s, n, "\r\n\r\n", 4);
}

// case-folded token compare: letters|0x20 lowercase; digits and '-' keep
// bit 5 already, so folding is lossless for HTTP token characters
static inline uint64_t fold8(const char* p) {
  uint64_t x;
  memcpy(&x, p, 8);
  return x | 0x2020202020202020ULL;
}
static inline uint64_t foldn(const char* p, size_t n) {  // 1 <= n < 8
  uint64_t x = 0;
  memcpy(&x, p, n);
  return (x | 0x2020202020202020ULL) & ((1ULL << (n * 8)) - 1);
}

// returns bytes consumed by one request, or 0 if incomplete / -1 bad
long http_parse(const char* s, size_t n, int* method, const char** path,
                size_t* plen, const char** auth, size_t* authn,
                const char** body, size_t* blen, int* minor,
                bool* connclose, bool* gz) {
  const char* he = hterm(s, n);
  if (UNLIKELY(!he)) return 0;
  size_t hdr = he + 4 - s;

  const char* sp = (const char*)memchr(s, ' ', hdr);
  if (UNLIKELY(!sp)) return -1;
  size_t ml = sp - s;
  if (ml == 3 && !memcmp(s, "GET", 3)) *method = M_GET;
  else if (ml == 4 && !memcmp(s, "POST", 4)) *method = M_POST;
  else *method = M_OTHER;
  const char* sp2 = (const char*)memchr(sp + 1, ' ', hdr);
  if (UNLIKELY(!sp2)) return -1;
  *path = sp + 1;
  *plen = sp2 - sp - 1;
  *minor = sp2 + 8 < he && sp2[8] == '0' ? 0 : 1;  // HTTP/1.x

  *auth = nullptr;
  *authn = 0;
  *gz = false;
  *connclose = *minor == 0;  // HTTP/1.0 closes unless keep-alive
  long long cl = 0;
  const char* p = (const char*)memchr(sp2, '\n', hdr - (sp2 - s));
  if (UNLIKELY(!p)) return -1;
  p++;
  // header lines occupy [p, he); the last one ends AT he (its \r\n is part of
  // the "\r\n\r\n" terminator), so it has no trailing CRLF inside the range.
  while (p < he) {
    const char* nl = (const char*)memchr(p, '\n', (size_t)(he - p));
    size_t ll = nl ? (size_t)(nl - p) : (size_t)(he - p);
    if (ll && p[ll - 1] == '\r') ll--;
    const char* colon = (const char*)memchr(p, ':', ll);
    if (colon) {
      size_t kn = colon - p;
      const char* v = colon + 1;
      size_t vn = ll - kn - 1;
      while (vn && (*v == ' ' || *v == '\t')) {
        v++;
        vn--;
      }
      while (vn && (v[vn - 1] == ' ' || v[vn - 1] == '\t')) vn--;
      if (kn == 15 && fold8(p) == 0x652D747065636361ULL &&
          foldn(p + 8, 7) == 0x676E69646F636EULL) {  // accept-encoding
        for (size_t k = 0; k + 4 <= vn; k++) {
          uint32_t x;
          memcpy(&x, v + k, 4);
          if ((x | 0x20202020u) == 0x70697A67u) {  // "gzip"
            *gz = true;
            break;
          }
        }
      } else if (kn == 13 && fold8(p) == 0x7A69726F68747561ULL &&
                 foldn(p + 8, 5) == 0x6E6F697461ULL) {  // authorization
        *auth = v;
        *authn = vn;
      } else if (kn == 14 && fold8(p) == 0x2D746E65746E6F63ULL &&
                 foldn(p + 8, 6) == 0x6874676E656CULL) {  // content-length
        long long x = 0;
        for (size_t k = 0; k < vn; k++) {
          unsigned d = (uint8_t)v[k] - '0';
          if (d > 9) break;  // strtoll semantics: stop at first non-digit
          x = x * 10 + d;
        }
        cl = x;
      } else if (kn == 10 && fold8(p) == 0x697463656E6E6F63ULL &&
                 foldn(p + 8, 2) == 0x6E6FULL) {  // connection
        if (vn == 5 && foldn(v, 5) == 0x65736F6C63ULL)
          *connclose = true;  // "close"
        else if (vn == 10 && fold8(v) == 0x696C612D7065656BULL &&
                 foldn(v + 8, 2) == 0x6576ULL)
          *connclose = false;  // "keep-alive"
      }
    }
    p = nl ? nl + 1 : he;
  }
  if (UNLIKELY((size_t)(n - hdr) < (size_t)cl)) return 0;  // body still arriving
  if (UNLIKELY(cl > (1 << 20))) return -1;
  *body = he + 4;
  *blen = cl;
  return (long)(hdr + cl);
}
