// The reply encoder of host/epoll.c (docs/specs/S22-the-encoder.md), a C port of
// vendor/million-users-challenge/submissions/csharp-epoll-xeonfx/Gzip.cs. Outside every proof.
//
// gz_compress writes one gzip member (RFC 1952): a 10-byte header, one deflate block with the
// fixed Huffman codes (RFC 1951 3.2.6, BTYPE 01), the CRC32 and the length. The block holds
// greedy LZ77 matches of 4 to 258 bytes within 32 KB, found through a table of 8,192 heads
// indexed by a hash of the next 4 bytes. A feed is a few KB of JSON that repeats itself twenty
// times, so the matches carry it, and building Huffman codes per reply costs more than it saves.
//
// The table is never cleared between calls. GzState.base is added to every position a call
// writes and rises by n + 32769 per call, so an entry from an earlier call is more than 32,768
// behind any position of this one and fails the same distance check as an empty entry. The table
// is cleared only when base would pass UINT32_MAX (about every 116,000 replies of 4 KB). A zeroed
// GzState is ready to use.
//
// CRC32 is gzip's (the reflected polynomial 0xEDB88320). The x86 crc32 instruction computes
// CRC32C, another polynomial, so it is not used: PCLMULQDQ folds 16 bytes at a time when the
// CPU has it (gz_init checks), and slice-by-8 tables take the rest.
//
// No allocation per call: the table and base live in the caller's GzState, the bit writer's
// state is local to the call, and the code tables are static, built once by gz_init.
#ifndef BM_GZIP_H
#define BM_GZIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#if defined(__x86_64__)
#include <immintrin.h>
#endif

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "host/gzip.h writes its bit stream and trailer little-endian"
#endif

#define GZ_HASH_BITS 13
#define GZ_MIN_MATCH 4u
#define GZ_MAX_MATCH 258u
#define GZ_MAX_DIST  32768u
#define GZ_MAX_IN    (1u << 30)  // a longer input gets 0: the caller sends it as it is

typedef struct {
  uint32_t head[1u << GZ_HASH_BITS];  // per hash: the last position seen, plus its call's base
  uint32_t base;                      // this call's offset; 0 before the first call
} GzState;

// The most gz_compress can write for n bytes: every byte a 9-bit literal, plus the header, the
// trailer and 8 bytes for the bit writer's last 64-bit store.
static size_t gz_bound(size_t n) {
  return n + n / 8 + 64;
}

// ---- the code tables (gz_init) ----

// A code is bit-reversed for the LSB-first writer, with its length in bits in the top byte.
static uint32_t gz_lit[256];                 // literal bytes
static uint32_t gz_len[GZ_MAX_MATCH + 1];    // match lengths 3..258: symbol code and extra bits
static uint32_t gz_dcode[30];                // distance codes 0..29 (5 bits)
static uint8_t  gz_dslot[512];               // a distance's code: [d - 1] under 256, [256 + ((d - 1) >> 7)] from there
static uint32_t gz_dinfo[30];                // per code: its 5 bits, and 5 + its extra bits << 24
static uint32_t gz_dbase[30];                // per code: its first distance, less 1
static uint32_t gz_crc[8][256];              // slice-by-8
static bool     gz_clmul;                    // the CPU has PCLMULQDQ and SSE4.1

static uint32_t gz_rev(uint32_t code, int len) {
  uint32_t r = 0;
  for (int k = 0; k < len; k += 1) {
    r |= ((code >> k) & 1u) << (len - 1 - k);
  }
  return r;
}

// The fixed code of literal/length symbol s (RFC 1951 3.2.6).
static uint32_t gz_symbol(uint32_t s) {
  if (s < 144) return gz_rev(0x30 + s, 8) | 8u << 24;
  if (s < 256) return gz_rev(0x190 + s - 144, 9) | 9u << 24;
  if (s < 280) return gz_rev(s - 256, 7) | 7u << 24;
  return gz_rev(0xC0 + s - 280, 8) | 8u << 24;
}

static void gz_init(void) {
  static const uint32_t bases[29]  = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35,
                                       43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
  static const uint32_t extras[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                       4, 4, 4, 4, 5, 5, 5, 5, 0 };
  for (uint32_t s = 0; s < 256; s += 1) {
    gz_lit[s] = gz_symbol(s);
  }
  for (uint32_t l = 3; l <= GZ_MAX_MATCH; l += 1) {
    uint32_t k = 28;
    while (bases[k] > l) k -= 1;
    if (l == 258) k = 28;  // 258 has its own symbol, 285, not 284 with extra bits 31
    uint32_t c   = gz_symbol(257 + k);
    uint32_t len = c >> 24;
    gz_len[l]    = (c & 0xFFFFFFu) | (l - bases[k]) << len | (len + extras[k]) << 24;
  }
  for (uint32_t c = 0; c < 30; c += 1) {
    uint32_t xbits = c < 4 ? 0 : (c >> 1) - 1;
    gz_dcode[c] = gz_rev(c, 5);
    gz_dinfo[c] = gz_dcode[c] | (5 + xbits) << 24;
    gz_dbase[c] = c < 4 ? c : (2u | (c & 1u)) << xbits;
    // a code from 16 on (base 256) spans a multiple of 128 distances, so the high bits name it
    for (uint32_t d = gz_dbase[c]; d < gz_dbase[c] + (1u << xbits); d += 1) {
      gz_dslot[d < 256 ? d : 256 + (d >> 7)] = (uint8_t)c;
    }
  }
  for (uint32_t b = 0; b < 256; b += 1) {
    uint32_t c = b;
    for (int k = 0; k < 8; k += 1) {
      c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    gz_crc[0][b] = c;
  }
  for (uint32_t b = 0; b < 256; b += 1) {
    for (int t = 1; t < 8; t += 1) {
      gz_crc[t][b] = (gz_crc[t - 1][b] >> 8) ^ gz_crc[0][gz_crc[t - 1][b] & 0xFF];
    }
  }
#if defined(__x86_64__)
  __builtin_cpu_init();
  gz_clmul = __builtin_cpu_supports("pclmul") && __builtin_cpu_supports("sse4.1");
#endif
}

// ---- loads and stores (unaligned, little-endian) ----

static inline uint32_t gz_ld32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

static inline uint64_t gz_ld64(const uint8_t* p) {
  uint64_t v;
  memcpy(&v, p, 8);
  return v;
}

static inline void gz_st64(uint8_t* p, uint64_t v) {
  memcpy(p, &v, 8);
}

static inline void gz_st32(uint8_t* p, uint32_t v) {
  memcpy(p, &v, 4);
}

// ---- CRC32 ----
// Both take and return the CRC register (the running value before the final inversion).

__attribute__((noinline)) static uint32_t gz_crc32_table(uint32_t crc, const uint8_t* p, size_t n) {
  for (; n >= 8; n -= 8, p += 8) {
    uint32_t a = gz_ld32(p) ^ crc, b = gz_ld32(p + 4);
    crc = gz_crc[7][a & 0xFF] ^ gz_crc[6][(a >> 8) & 0xFF] ^ gz_crc[5][(a >> 16) & 0xFF] ^ gz_crc[4][a >> 24]
      ^ gz_crc[3][b & 0xFF] ^ gz_crc[2][(b >> 8) & 0xFF] ^ gz_crc[1][(b >> 16) & 0xFF] ^ gz_crc[0][b >> 24];
  }
  for (; n > 0; n -= 1, p += 1) {
    crc = (crc >> 8) ^ gz_crc[0][(crc ^ *p) & 0xFF];
  }
  return crc;
}

#if defined(__x86_64__)
// "Fast CRC Computation for Generic Polynomials Using PCLMULQDQ Instruction" (Gopal, Ozturk et
// al., Intel, 2009), with the bit-reflected constants for 0xEDB88320 that zlib's Chromium fork
// uses (crc32_simd.c). Folds four 16-byte lanes 64 bytes at a time, then one lane 16 at a time,
// then reduces 128 bits to 32. The bytes past the last multiple of 16 go through the table.
__attribute__((noinline, target("pclmul,sse4.1")))
static uint32_t gz_crc32_clmul(uint32_t crc, const uint8_t* p, size_t n) {
  if (n < 64) {
    return gz_crc32_table(crc, p, n);
  }
  const __m128i k1k2 = _mm_set_epi64x(0x01c6e41596, 0x0154442bd4);
  const __m128i k3k4 = _mm_set_epi64x(0x00ccaa009e, 0x01751997d0);
  const __m128i k5k0 = _mm_set_epi64x(0, 0x0163cd6124);
  const __m128i poly = _mm_set_epi64x(0x01f7011641, 0x01db710641);
  __m128i x1 = _mm_loadu_si128((const __m128i*)(p + 0x00));
  __m128i x2 = _mm_loadu_si128((const __m128i*)(p + 0x10));
  __m128i x3 = _mm_loadu_si128((const __m128i*)(p + 0x20));
  __m128i x4 = _mm_loadu_si128((const __m128i*)(p + 0x30));
  x1 = _mm_xor_si128(x1, _mm_cvtsi32_si128((int)crc));
  p += 64;
  n -= 64;
  __m128i x0 = k1k2, x5, x6, x7, x8;
  for (; n >= 64; n -= 64, p += 64) {
    x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
    x6 = _mm_clmulepi64_si128(x2, x0, 0x00);
    x7 = _mm_clmulepi64_si128(x3, x0, 0x00);
    x8 = _mm_clmulepi64_si128(x4, x0, 0x00);
    x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
    x2 = _mm_clmulepi64_si128(x2, x0, 0x11);
    x3 = _mm_clmulepi64_si128(x3, x0, 0x11);
    x4 = _mm_clmulepi64_si128(x4, x0, 0x11);
    x1 = _mm_xor_si128(_mm_xor_si128(x1, x5), _mm_loadu_si128((const __m128i*)(p + 0x00)));
    x2 = _mm_xor_si128(_mm_xor_si128(x2, x6), _mm_loadu_si128((const __m128i*)(p + 0x10)));
    x3 = _mm_xor_si128(_mm_xor_si128(x3, x7), _mm_loadu_si128((const __m128i*)(p + 0x20)));
    x4 = _mm_xor_si128(_mm_xor_si128(x4, x8), _mm_loadu_si128((const __m128i*)(p + 0x30)));
  }
  // the four lanes into one
  x0 = k3k4;
  x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
  x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
  x1 = _mm_xor_si128(_mm_xor_si128(x1, x2), x5);
  x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
  x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
  x1 = _mm_xor_si128(_mm_xor_si128(x1, x3), x5);
  x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
  x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
  x1 = _mm_xor_si128(_mm_xor_si128(x1, x4), x5);
  for (; n >= 16; n -= 16, p += 16) {
    x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
    x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
    x1 = _mm_xor_si128(_mm_xor_si128(x1, _mm_loadu_si128((const __m128i*)p)), x5);
  }
  // 128 bits to 64
  const __m128i lo32 = _mm_setr_epi32(~0, 0, ~0, 0);
  x2 = _mm_clmulepi64_si128(x1, x0, 0x10);
  x1 = _mm_xor_si128(_mm_srli_si128(x1, 8), x2);
  x2 = _mm_srli_si128(x1, 4);
  x1 = _mm_clmulepi64_si128(_mm_and_si128(x1, lo32), k5k0, 0x00);
  x1 = _mm_xor_si128(x1, x2);
  // Barrett reduction to 32 bits
  x2 = _mm_clmulepi64_si128(_mm_and_si128(x1, lo32), poly, 0x10);
  x2 = _mm_clmulepi64_si128(_mm_and_si128(x2, lo32), poly, 0x00);
  x1 = _mm_xor_si128(x1, x2);
  crc = (uint32_t)_mm_extract_epi32(x1, 1);
  return gz_crc32_table(crc, p, n);
}
#endif

// gzip's CRC32 of p[0 .. n - 1].
static uint32_t gz_crc32(const uint8_t* p, size_t n) {
#if defined(__x86_64__)
  if (gz_clmul) {
    return ~gz_crc32_clmul(0xFFFFFFFFu, p, n);
  }
#endif
  return ~gz_crc32_table(0xFFFFFFFFu, p, n);
}

// ---- the deflate block ----

// Writes n literals, four at a time: their codes joined (at most 36 bits, 43 with the bits
// waiting) and stored once. For the bytes after the last position a match may start at; the
// literals before that go out one by one in gz_compress's loop.
__attribute__((always_inline)) static inline uint8_t* gz_literals(const uint8_t* p, size_t n, uint8_t* op,
  uint64_t* bits, uint32_t* count) {
  uint64_t b = *bits;
  uint32_t c = *count;
  const uint8_t* end = p + n;
  for (; end - p >= 4; p += 4) {
    uint32_t c0 = gz_lit[p[0]], c1 = gz_lit[p[1]], c2 = gz_lit[p[2]], c3 = gz_lit[p[3]];
    uint32_t l0 = c0 >> 24, l1 = c1 >> 24, l2 = c2 >> 24, l3 = c3 >> 24;
    uint64_t joined = (uint64_t)(c0 & 0xFFFFFFu) | (uint64_t)(c1 & 0xFFFFFFu) << l0
      | (uint64_t)(c2 & 0xFFFFFFu) << (l0 + l1) | (uint64_t)(c3 & 0xFFFFFFu) << (l0 + l1 + l2);
    b |= joined << c;
    c += l0 + l1 + l2 + l3;
    gz_st64(op, b);
    op += c >> 3;
    b >>= c & ~7u;
    c &= 7;
  }
  for (; p < end; p += 1) {
    uint32_t c0 = gz_lit[*p];
    b |= (uint64_t)(c0 & 0xFFFFFFu) << c;
    c += c0 >> 24;
    gz_st64(op, b);
    op += c >> 3;
    b >>= c & ~7u;
    c &= 7;
  }
  *bits  = b;
  *count = c;
  return op;
}

// A distance's code and extra bits (LSB first), with their count in the top byte. The code comes
// from gz_dslot (zlib's two-level table: a distance under 257 by itself, a longer one by its
// high bits), not from a count of leading zeros: bsr is slow on the CPUs without lzcnt in
// their baseline, and this runs once a match.
__attribute__((always_inline)) static inline uint32_t gz_dist(uint32_t distance) {
  uint32_t d    = distance - 1;
  uint32_t code = gz_dslot[d < 256 ? d : 256 + (d >> 7)];
  return gz_dinfo[code] + ((d - gz_dbase[code]) << 5);
}

static inline uint32_t gz_hash(uint32_t v) {
  return (v * 2654435761u) >> (32 - GZ_HASH_BITS);
}

// Writes src[0 .. n - 1] as one gzip member into dst; answers its length, or 0 when cap is under
// gz_bound(n) or n is over GZ_MAX_IN. gz_init must have run.
__attribute__((noinline)) static size_t gz_compress(GzState* g, const uint8_t* src, size_t n, uint8_t* dst,
  size_t cap) {
  if (n > GZ_MAX_IN || cap < gz_bound(n)) {
    return 0;
  }
  uint32_t len = (uint32_t)n;
  if (g->base <= GZ_MAX_DIST || g->base > UINT32_MAX - len - GZ_MAX_DIST - 1) {
    memset(g->head, 0, sizeof g->head);
    g->base = GZ_MAX_DIST + 1;
  }
  uint32_t  bas  = g->base;
  uint32_t* head = g->head;
  g->base += len + GZ_MAX_DIST + 1;

  // magic, deflate, no flags, no mtime, no extra flags, OS unknown
  static const uint8_t header[10] = { 0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0xFF };
  memcpy(dst, header, sizeof header);
  uint8_t* op    = dst + 10;
  uint64_t bits  = 3;  // BFINAL 1, BTYPE 01 (fixed codes)
  uint32_t count = 3;
  uint32_t i = 0;
  if (len >= GZ_MIN_MATCH) {
    uint32_t anchor = len - GZ_MIN_MATCH;  // a later position cannot start a match
    while (i <= anchor) {
      uint32_t v = gz_ld32(src + i);
      uint32_t h = gz_hash(v);
      // An entry of this call is 1 to i behind; one from an earlier call, or 0, more than
      // GZ_MAX_DIST: one range check refuses both.
      uint32_t dist = i + bas - head[h];
      head[h]       = i + bas;
      if (dist - 1 >= GZ_MAX_DIST || gz_ld32(src + i - dist) != v) {
        // No match here: the byte goes out as a literal at once. Its code is one load from the
        // byte already in v, and a run of literals costs no second pass and no loop of its own
        // (most runs before a match are of 0 to 3 bytes).
        uint32_t c0 = gz_lit[v & 0xFFu];
        bits |= (uint64_t)(c0 & 0xFFFFFFu) << count;
        count += c0 >> 24;
        gz_st64(op, bits);
        op += count >> 3;
        bits >>= count & ~7u;
        count &= 7;
        i += 1;
        continue;
      }
      const uint8_t* cand  = src + i - dist;
      uint32_t       m     = GZ_MIN_MATCH;
      uint32_t       limit = len - i < GZ_MAX_MATCH ? len - i : GZ_MAX_MATCH;
      for (;;) {
        if (m + 8 > limit) {
          while (m < limit && src[i + m] == cand[m]) m += 1;
          break;
        }
        uint64_t x = gz_ld64(src + i + m) ^ gz_ld64(cand + m);
        if (x != 0) {
          m += (uint32_t)__builtin_ctzll(x) >> 3;
          break;
        }
        m += 8;
      }
      uint32_t l = gz_len[m];
      bits |= (uint64_t)(l & 0xFFFFFFu) << count;
      count += l >> 24;
      uint32_t d = gz_dist(dist);
      bits |= (uint64_t)(d & 0xFFFFFFu) << count;
      count += d >> 24;
      gz_st64(op, bits);
      op += count >> 3;
      bits >>= count & ~7u;
      count &= 7;
      i += m;
      // the match's last position goes in too, so the next post's copy of it finds this one
      if (i <= anchor) {
        head[gz_hash(gz_ld32(src + i - 1))] = i - 1 + bas;
      }
    }
  }
  op = gz_literals(src + i, len - i, op, &bits, &count);
  // end of block (symbol 256: seven 0 bits), then pad to a byte
  count += 7;
  gz_st64(op, bits);
  op += (count + 7) >> 3;
  gz_st32(op, gz_crc32(src, n));
  gz_st32(op + 4, len);
  return (size_t)(op + 8 - dst);
}

#endif
