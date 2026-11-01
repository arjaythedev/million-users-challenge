// Minimal JSON support: a cursor that can walk a flat object, read a
// string span, and skip any value; plus unescape/escape helpers and the
// {"body": "..."} extractor used by POST /posts. No allocation anywhere.

#include "server.h"

#include <emmintrin.h>

void jws(Jc& j) {
  while (j.p < j.e && (*j.p == ' ' || *j.p == '\t' || *j.p == '\n' ||
                       *j.p == '\r'))
    j.p++;
}

// parse a JSON string literal; *s/*n get the raw span between quotes
// (escapes kept). returns false on malformed input.
bool jstr(Jc& j, const char** s, uint32_t* n) {
  if (j.p >= j.e || *j.p != '"') return false;
  const char* start = ++j.p;
  while (j.p < j.e) {
    char c = *j.p;
    if (c == '"') {
      *s = start;
      *n = (uint32_t)(j.p - start);
      j.p++;
      return true;
    }
    if (c == '\\') {
      if (++j.p >= j.e) return false;
      if (*j.p == 'u') {
        for (int i = 0; i < 4; i++)
          if (++j.p >= j.e || !isxdigit((uint8_t)*j.p)) return false;
      }
      j.p++;
      continue;
    }
    if ((uint8_t)c < 0x20) return false;
    j.p++;
  }
  return false;
}

// skip any value: string, number, true/false/null, object, array
bool jskip(Jc& j) {
  jws(j);
  if (j.p >= j.e) return false;
  char c = *j.p;
  if (c == '"') {
    const char* s;
    uint32_t n;
    return jstr(j, &s, &n);
  }
  if (c == '{' || c == '[') {
    int depth = 0;
    do {
      if (j.p >= j.e) return false;
      c = *j.p;
      if (c == '"') {
        const char* s;
        uint32_t n;
        if (!jstr(j, &s, &n)) return false;
        continue;
      }
      if (c == '{' || c == '[') depth++;
      else if (c == '}' || c == ']') depth--;
      else if ((uint8_t)c < 0x20 && c != ' ' && c != '\t' && c != '\n' &&
               c != '\r')
        return false;
      j.p++;
    } while (depth > 0);
    return true;
  }
  // scalar: number, true, false, null - consume until delimiter
  const char* start = j.p;
  while (j.p < j.e) {
    c = *j.p;
    if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' ||
        c == '\n' || c == '\r')
      break;
    j.p++;
  }
  return j.p > start;
}

// unescape a raw JSON string span into dst (cap bytes). UTF-16 escapes are
// encoded to UTF-8. returns length, or -1 on bad escape.
int junesc(char* dst, size_t cap, const char* s, size_t n) {
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    uint8_t c = (uint8_t)s[i];
    if (c != '\\') {
      if (o >= cap) return -1;
      dst[o++] = c;
      continue;
    }
    if (++i >= n) return -1;
    c = s[i];
    char m;
    switch (c) {
      case '"': m = '"'; break;
      case '\\': m = '\\'; break;
      case '/': m = '/'; break;
      case 'b': m = '\b'; break;
      case 'f': m = '\f'; break;
      case 'n': m = '\n'; break;
      case 'r': m = '\r'; break;
      case 't': m = '\t'; break;
      case 'u': {
        if (i + 4 >= n) return -1;
        auto hex = [&](int k) -> int {
          char h = s[i + k];
          if (h >= '0' && h <= '9') return h - '0';
          if (h >= 'a' && h <= 'f') return h - 'a' + 10;
          if (h >= 'A' && h <= 'F') return h - 'A' + 10;
          return -1;
        };
        int h0 = hex(1), h1 = hex(2), h2 = hex(3), h3 = hex(4);
        if ((h0 | h1 | h2 | h3) < 0) return -1;
        uint32_t cp = h0 << 12 | h1 << 8 | h2 << 4 | h3;
        i += 4;
        if (cp >= 0xd800 && cp <= 0xdbff && i + 5 < n && s[i + 1] == '\\' &&
            s[i + 2] == 'u') {
          int g0 = hex(3), g1 = hex(4), g2 = hex(5), g3 = hex(6);
          if ((g0 | g1 | g2 | g3) >= 0) {
            uint32_t lo = g0 << 12 | g1 << 8 | g2 << 4 | g3;
            if (lo >= 0xdc00 && lo <= 0xdfff) {
              cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
              i += 6;
            }
          }
        }
        if (cp < 0x80) {
          if (o + 1 > cap) return -1;
          dst[o++] = cp;
        } else if (cp < 0x800) {
          if (o + 2 > cap) return -1;
          dst[o++] = 0xc0 | cp >> 6;
          dst[o++] = 0x80 | (cp & 63);
        } else if (cp < 0x10000) {
          if (o + 3 > cap) return -1;
          dst[o++] = 0xe0 | cp >> 12;
          dst[o++] = 0x80 | (cp >> 6 & 63);
          dst[o++] = 0x80 | (cp & 63);
        } else {
          if (o + 4 > cap) return -1;
          dst[o++] = 0xf0 | cp >> 18;
          dst[o++] = 0x80 | (cp >> 12 & 63);
          dst[o++] = 0x80 | (cp >> 6 & 63);
          dst[o++] = 0x80 | (cp & 63);
        }
        continue;
      }
      default: return -1;
    }
    if (o >= cap) return -1;
    dst[o++] = m;
  }
  return (int)o;
}

// count UTF-8 codepoints in a byte span (Postgres char_length semantics)
size_t utf8_count(const char* s, size_t n) {
  size_t c = 0;
  for (size_t i = 0; i < n; i++)
    if (((uint8_t)s[i] & 0xc0) != 0x80) c++;
  return c;
}

// emit escaped byte c at o; false if it does not fit
static inline bool jesc_byte(uint8_t c, char* d, size_t& o, size_t cap) {
  static const char hx[] = "0123456789abcdef";
  if (c == '"' || c == '\\') {
    if (o + 2 > cap) return false;
    d[o] = '\\';
    d[o + 1] = (char)c;
    o += 2;
  } else {
    if (o + 6 > cap) return false;
    d[o] = '\\';
    d[o + 1] = 'u';
    d[o + 2] = '0';
    d[o + 3] = '0';
    d[o + 4] = hx[c >> 4];
    d[o + 5] = hx[c & 15];
    o += 6;
  }
  return true;
}

static size_t jesc_tail(char* d, size_t cap, const char* s, size_t n, size_t o,
                        size_t i) {
  for (; i < n; i++) {
    uint8_t c = (uint8_t)s[i];
    if (c == '"' || c == '\\' || c < 0x20) {
      if (!jesc_byte(c, d, o, cap)) return SIZE_MAX;
    } else {
      if (o + 1 > cap) return SIZE_MAX;
      d[o++] = (char)c;
    }
  }
  return o;
}

#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
static const bool g_avx2 = __builtin_cpu_supports("avx2") != 0;

__attribute__((target("avx2"))) static size_t
jesc_avx2(char* d, size_t cap, const char* s, size_t n) {
  size_t o = 0, i = 0;
  const __m256i q = _mm256_set1_epi8('"');
  const __m256i bs = _mm256_set1_epi8('\\');
  const __m256i lo = _mm256_set1_epi8(0x1f);
  while (i + 32 <= n) {
    __m256i v = _mm256_loadu_si256((const __m256i*)(s + i));
    __m256i bad = _mm256_or_si256(
        _mm256_or_si256(_mm256_cmpeq_epi8(v, q), _mm256_cmpeq_epi8(v, bs)),
        _mm256_cmpeq_epi8(_mm256_min_epu8(v, lo), v));
    unsigned m = (unsigned)_mm256_movemask_epi8(bad);
    if (!m) {
      if (o + 32 > cap) return SIZE_MAX;
      memcpy(d + o, s + i, 32);
      o += 32;
      i += 32;
      continue;
    }
    int tz = __builtin_ctz(m);
    for (int k = 0; k < tz; k++) {
      if (o >= cap) return SIZE_MAX;
      d[o++] = s[i + k];
    }
    i += tz;
    uint8_t c = (uint8_t)s[i++];
    if (!jesc_byte(c, d, o, cap)) return SIZE_MAX;
  }
  return jesc_tail(d, cap, s, n, o, i);
}
#endif

size_t jesc(char* d, size_t cap, const char* s, size_t n) {
#if defined(__x86_64__) && defined(__GNUC__)
  if (g_avx2) return jesc_avx2(d, cap, s, n);
#endif
  size_t o = 0, i = 0;
  const __m128i q = _mm_set1_epi8('"');
  const __m128i bs = _mm_set1_epi8('\\');
  const __m128i lo = _mm_set1_epi8(0x1f);
  // bulk-copy 16-byte spans with no byte needing an escape
  while (i + 16 <= n) {
    __m128i v = _mm_loadu_si128((const __m128i*)(s + i));
    __m128i bad = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, q),
                                            _mm_cmpeq_epi8(v, bs)),
                               _mm_cmpeq_epi8(_mm_min_epu8(v, lo), v));
    int m = _mm_movemask_epi8(bad);
    if (!m) {
      if (o + 16 > cap) return SIZE_MAX;
      memcpy(d + o, s + i, 16);
      o += 16;
      i += 16;
      continue;
    }
    int tz = __builtin_ctz((unsigned)m);
    for (int k = 0; k < tz; k++) {
      if (o >= cap) return SIZE_MAX;
      d[o++] = s[i + k];
    }
    i += tz;
    uint8_t c = (uint8_t)s[i++];
    if (!jesc_byte(c, d, o, cap)) return SIZE_MAX;
  }
  return jesc_tail(d, cap, s, n, o, i);
}

// extract {"body": "..."} from a JSON body.
// returns 0 got-string / 1 missing-or-not-string / 2 malformed
int json_body(const char* s, size_t n, char* dst, size_t cap,
              uint16_t* dlen) {
  Jc j{s, s + n};
  jws(j);
  if (j.p >= j.e || *j.p != '{') return 2;
  j.p++;
  bool found = false;
  for (;;) {
    jws(j);
    if (j.p >= j.e) return 2;
    if (*j.p == '}') return found ? 0 : 1;
    const char* k;
    uint32_t kn;
    if (!jstr(j, &k, &kn)) return 2;
    jws(j);
    if (j.p >= j.e || *j.p != ':') return 2;
    j.p++;
    jws(j);
    if (j.p < j.e && *j.p == '"') {
      const char* v;
      uint32_t vn;
      if (!jstr(j, &v, &vn)) return 2;
      if (kn == 4 && !memcmp(k, "body", 4) && !found) {
        int un = junesc(dst, cap, v, vn);
        if (un < 0) un = (int)cap;  // overflow -> treated as too long later
        *dlen = (uint16_t)un;
        found = true;
      }
    } else if (!jskip(j)) {
      return 2;
    }
    jws(j);
    if (j.p >= j.e) return 2;
    if (*j.p == ',') {
      j.p++;
      continue;
    }
    if (*j.p == '}') return found ? 0 : 1;
    return 2;
  }
}
