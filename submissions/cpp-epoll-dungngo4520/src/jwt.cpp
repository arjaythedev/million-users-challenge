// HS256 JWT verification: in-tree SHA-256, base64url decode, header and
// payload checks. Signature and exp are verified on every request; the
// results are never cached.

#include "server.h"

// ------------------------------------------------------------------ sha256

struct Sha256 {
  uint32_t h[8];
  uint64_t len;
  uint8_t buf[64];
  size_t n;
};

static const uint32_t SHK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
// hardware SHA-256 rounds (Intel SHA extensions); ~4-6x faster per block
__attribute__((target("sha,sse4.1,ssse3")))
static void sha_block_ni(Sha256* s, const uint8_t* p) {
  const __m128i MASK = _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);
  __m128i TMP = _mm_loadu_si128((const __m128i*)&s->h[0]);
  __m128i STATE1 = _mm_loadu_si128((const __m128i*)&s->h[4]);
  TMP = _mm_shuffle_epi32(TMP, 0xB1);          // CDAB
  STATE1 = _mm_shuffle_epi32(STATE1, 0x1B);    // HGFE
  __m128i STATE0 = _mm_alignr_epi8(TMP, STATE1, 8);   // ABEF
  STATE1 = _mm_blend_epi16(STATE1, TMP, 0xF0);        // CDGH
  __m128i ABEF_SAVE = STATE0, CDGH_SAVE = STATE1;
  __m128i MSG0 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(p + 0)), MASK);
  __m128i MSG1 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(p + 16)), MASK);
  __m128i MSG2 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(p + 32)), MASK);
  __m128i MSG3 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(p + 48)), MASK);
  __m128i MSG;
#define SH4(K, M)                                                        \
  MSG = _mm_add_epi32(M, _mm_loadu_si128((const __m128i*)&SHK[4 * (K)])); \
  STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);                    \
  MSG = _mm_shuffle_epi32(MSG, 0x0E);                                    \
  STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG)
// group i schedule: D=M[i%4] = msg2(D + alignr(M[(i+3)%4], M[(i+2)%4],4),
// M[(i+3)%4]). msg1(X, Y) for a future group may only run after every
// consumer of X's raw values: X=M[(g+2)%4], Y=M[(g+3)%4] at end of g>=5.
#define SCHED(D, A, B)                                                   \
  D = _mm_add_epi32(D, _mm_alignr_epi8(A, B, 4));                        \
  D = _mm_sha256msg2_epu32(D, A)
  SH4(0, MSG0);
  SH4(1, MSG1); MSG0 = _mm_sha256msg1_epu32(MSG0, MSG1);
  SH4(2, MSG2); MSG1 = _mm_sha256msg1_epu32(MSG1, MSG2);
  SH4(3, MSG3);
  SCHED(MSG0, MSG3, MSG2); SH4(4, MSG0); MSG2 = _mm_sha256msg1_epu32(MSG2, MSG3);
  SCHED(MSG1, MSG0, MSG3); SH4(5, MSG1); MSG3 = _mm_sha256msg1_epu32(MSG3, MSG0);
  SCHED(MSG2, MSG1, MSG0); SH4(6, MSG2); MSG0 = _mm_sha256msg1_epu32(MSG0, MSG1);
  SCHED(MSG3, MSG2, MSG1); SH4(7, MSG3); MSG1 = _mm_sha256msg1_epu32(MSG1, MSG2);
  SCHED(MSG0, MSG3, MSG2); SH4(8, MSG0); MSG2 = _mm_sha256msg1_epu32(MSG2, MSG3);
  SCHED(MSG1, MSG0, MSG3); SH4(9, MSG1); MSG3 = _mm_sha256msg1_epu32(MSG3, MSG0);
  SCHED(MSG2, MSG1, MSG0); SH4(10, MSG2); MSG0 = _mm_sha256msg1_epu32(MSG0, MSG1);
  SCHED(MSG3, MSG2, MSG1); SH4(11, MSG3); MSG1 = _mm_sha256msg1_epu32(MSG1, MSG2);
  SCHED(MSG0, MSG3, MSG2); SH4(12, MSG0); MSG2 = _mm_sha256msg1_epu32(MSG2, MSG3);
  SCHED(MSG1, MSG0, MSG3); SH4(13, MSG1); MSG3 = _mm_sha256msg1_epu32(MSG3, MSG0);
  SCHED(MSG2, MSG1, MSG0); SH4(14, MSG2);
  SCHED(MSG3, MSG2, MSG1); SH4(15, MSG3);
#undef SH4
#undef SCHED
  STATE0 = _mm_add_epi32(STATE0, ABEF_SAVE);
  STATE1 = _mm_add_epi32(STATE1, CDGH_SAVE);
  TMP = _mm_shuffle_epi32(STATE0, 0x1B);        // FEBA
  STATE1 = _mm_shuffle_epi32(STATE1, 0xB1);     // DCHG
  STATE0 = _mm_blend_epi16(TMP, STATE1, 0xF0);  // DCBA
  STATE1 = _mm_alignr_epi8(STATE1, TMP, 8);     // HGFE
  _mm_storeu_si128((__m128i*)&s->h[0], STATE0);
  _mm_storeu_si128((__m128i*)&s->h[4], STATE1);
}
static const bool g_sha_ni = __builtin_cpu_supports("sha") != 0;
#endif

static void sha_block(Sha256* s, const uint8_t* p) {
#if defined(__x86_64__) && defined(__GNUC__)
  if (g_sha_ni) {
    sha_block_ni(s, p);
    return;
  }
#endif
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
           (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t a = w[i - 15], b = w[i - 2];
    uint32_t s0 = (a >> 7 | a << 25) ^ (a >> 18 | a << 14) ^ (a >> 3);
    uint32_t s1 = (b >> 17 | b << 15) ^ (b >> 19 | b << 13) ^ (b >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t v[8];
  memcpy(v, s->h, 32);
  for (int i = 0; i < 64; i++) {
    uint32_t e = v[4], f = v[5], g = v[6];
    uint32_t S1 = (e >> 6 | e << 26) ^ (e >> 11 | e << 21) ^ (e >> 25 | e << 7);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = v[7] + S1 + ch + SHK[i] + w[i];
    uint32_t a = v[0], b = v[1], c = v[2];
    uint32_t S0 = (a >> 2 | a << 30) ^ (a >> 13 | a << 19) ^ (a >> 22 | a << 10);
    uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + mj;
    v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
    v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
  }
  for (int i = 0; i < 8; i++) s->h[i] += v[i];
}

static void sha_init(Sha256* s) {
  static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                 0xa54ff53a, 0x510e527f, 0x9b05688c,
                                 0x1f83d9ab, 0x5be0cd19};
  memcpy(s->h, iv, 32);
  s->len = 0;
  s->n = 0;
}

static void sha_upd(Sha256* s, const void* data, size_t n) {
  const uint8_t* p = (const uint8_t*)data;
  s->len += n;
  while (n) {
    size_t k = 64 - s->n;
    if (k > n) k = n;
    memcpy(s->buf + s->n, p, k);
    s->n += k;
    p += k;
    n -= k;
    if (s->n == 64) {
      sha_block(s, s->buf);
      s->n = 0;
    }
  }
}

static void sha_fin(Sha256* s, uint8_t out[32]) {
  uint64_t bits = s->len * 8;
  uint8_t pad = 0x80;
  sha_upd(s, &pad, 1);
  uint8_t z = 0;
  while (s->n != 56) sha_upd(s, &z, 1);
  uint8_t lb[8];
  for (int i = 0; i < 8; i++) lb[i] = bits >> (56 - 8 * i);
  sha_upd(s, lb, 8);
  for (int i = 0; i < 8; i++) {
    out[4 * i] = s->h[i] >> 24;
    out[4 * i + 1] = s->h[i] >> 16;
    out[4 * i + 2] = s->h[i] >> 8;
    out[4 * i + 3] = s->h[i];
  }
}

void hmac_sha256(const uint8_t* key, size_t klen, const uint8_t* msg,
                        size_t mlen, uint8_t out[32]) {
  uint8_t kb[64];
  memset(kb, 0, 64);
  if (klen > 64) {
    Sha256 t;
    sha_init(&t);
    sha_upd(&t, key, klen);
    sha_fin(&t, kb);
  } else {
    memcpy(kb, key, klen);
  }
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; i++) {
    ipad[i] = kb[i] ^ 0x36;
    opad[i] = kb[i] ^ 0x5c;
  }
  Sha256 s;
  uint8_t inner[32];
  sha_init(&s);
  sha_upd(&s, ipad, 64);
  sha_upd(&s, msg, mlen);
  sha_fin(&s, inner);
  sha_init(&s);
  sha_upd(&s, opad, 64);
  sha_upd(&s, inner, 32);
  sha_fin(&s, out);
}

// ------------------------------------------------------------------ base64url

static int b64v(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-' || c == '+') return 62;
  if (c == '_' || c == '/') return 63;
  return -1;
}

// returns decoded length or -1
int b64_dec(const char* s, size_t n, uint8_t* out) {
  while (n && s[n - 1] == '=') n--;
  if (n == 0 || n % 4 == 1) return -1;
  size_t o = 0, i = 0;
  while (i + 4 <= n) {
    int a = b64v(s[i]), b = b64v(s[i + 1]), c = b64v(s[i + 2]),
        d = b64v(s[i + 3]);
    if ((a | b | c | d) < 0) return -1;
    out[o++] = a << 2 | b >> 4;
    out[o++] = b << 4 | c >> 2;
    out[o++] = c << 6 | d;
    i += 4;
  }
  if (n - i == 2) {
    int a = b64v(s[i]), b = b64v(s[i + 1]);
    if ((a | b) < 0 || (b & 15)) return -1;
    out[o++] = a << 2 | b >> 4;
  } else if (n - i == 3) {
    int a = b64v(s[i]), b = b64v(s[i + 1]), c = b64v(s[i + 2]);
    if ((a | b | c) < 0 || (c & 3)) return -1;
    out[o++] = a << 2 | b >> 4;
    out[o++] = b << 4 | c >> 2;
  }
  return (int)o;
}

// ------------------------------------------------------------------ jwt

// verify an HS256 token span (no "Bearer " prefix).
// returns 0 ok / 1 invalid-or-expired / 2 invalid-payload.
// on ok: *uid set; uname/ulen filled with the unescaped username.
static int jwt_check(const char* t, size_t n, int64_t* uid, char* uname,
                     size_t ucap, uint16_t* ulen) {
  const char* d1 = (const char*)memchr(t, '.', n);
  if (!d1 || d1 == t) return 1;
  const char* d2 = (const char*)memchr(d1 + 1, '.', n - (d1 + 1 - t));
  if (!d2 || d2 == d1 + 1 || d2 == t + n - 1) return 1;
  if (memchr(d2 + 1, '.', n - (d2 + 1 - t))) return 1;
  size_t hlen = d1 - t, plen = d2 - d1 - 1, slen = n - (d2 + 1 - t);

  uint8_t hbuf[1024];
  if (hlen > 1020) return 1;
  int hn = b64_dec(t, hlen, hbuf);
  if (hn <= 0) return 1;

  // header must be an object containing "alg":"HS256"
  {
    Jc j{ (const char*)hbuf, (const char*)hbuf + hn };
    jws(j);
    if (j.p >= j.e || *j.p != '{') return 1;
    j.p++;
    bool ok = false;
    for (;;) {
      jws(j);
      if (j.p < j.e && *j.p == '}') break;
      const char* k;
      uint32_t kn;
      if (!jstr(j, &k, &kn)) return 1;
      jws(j);
      if (j.p >= j.e || *j.p != ':') return 1;
      j.p++;
      if (kn == 3 && !memcmp(k, "alg", 3)) {
        const char* v;
        uint32_t vn;
        jws(j);
        if (!jstr(j, &v, &vn)) return 1;
        ok = vn == 5 && !memcmp(v, "HS256", 5);
      } else if (!jskip(j)) {
        return 1;
      }
      jws(j);
      if (j.p < j.e && *j.p == ',') {
        j.p++;
        continue;
      }
      break;
    }
    if (!ok) return 1;
  }

  // signature
  uint8_t sig[512];
  if (slen > 500) return 1;
  int sn = b64_dec(d2 + 1, slen, sig);
  if (sn != 32) return 1;
  uint8_t mac[32];
  hmac_sha256((const uint8_t*)g_secret, g_secret_len, (const uint8_t*)t,
              d2 - t, mac);
  uint8_t diff = 0;
  for (int i = 0; i < 32; i++) diff |= sig[i] ^ mac[i];
  if (diff) return 1;

  // payload: decode on the stack; it is bounded by the request size
  uint8_t pbuf[16384];
  if (plen > 16384) return 1;
  int pn = b64_dec(d1 + 1, plen, pbuf);
  if (pn <= 0) return 1;
  int rc = 1;
  bool got_exp = false, got_sub = false, got_name = false;
  double expv = 0;
  const char* subs = nullptr;
  uint32_t subn = 0;
  const char* names = nullptr;
  uint32_t namen = 0;
  {
    Jc j{ (const char*)pbuf, (const char*)pbuf + pn };
    jws(j);
    if (j.p >= j.e || *j.p != '{') return 1;
    j.p++;
    for (;;) {
      jws(j);
      if (j.p < j.e && *j.p == '}') break;
      const char* k;
      uint32_t kn;
      if (!jstr(j, &k, &kn)) return 1;
      jws(j);
      if (j.p >= j.e || *j.p != ':') return 1;
      j.p++;
      jws(j);
      if (kn == 3 && !memcmp(k, "exp", 3)) {
        // bounded number parse (payload is not NUL-terminated)
        bool neg = false;
        const char* q = j.p;
        if (q < j.e && *q == '-') {
          neg = true;
          q++;
        }
        if (q >= j.e || !isdigit((uint8_t)*q)) return 1;
        double v = 0;
        while (q < j.e && isdigit((uint8_t)*q)) v = v * 10 + (*q++ - '0');
        if (q < j.e && *q == '.') {
          q++;
          double f = 0.1;
          while (q < j.e && isdigit((uint8_t)*q)) {
            v += (*q++ - '0') * f;
            f /= 10;
          }
        }
        expv = neg ? -v : v;
        j.p = q;
        got_exp = true;
      } else if (kn == 3 && !memcmp(k, "sub", 3)) {
        if (!jstr(j, &subs, &subn)) return 1;
        got_sub = true;
      } else if (kn == 8 && !memcmp(k, "username", 8)) {
        if (!jstr(j, &names, &namen)) return 1;
        got_name = true;
      } else if (!jskip(j)) {
        return 1;
      }
      jws(j);
      if (j.p < j.e && *j.p == ',') {
        j.p++;
        continue;
      }
      break;
    }
  }
  if (!got_exp || expv <= (double)time(nullptr)) {
    return 1;
  }
  if (!got_sub || !got_name) {
    return 2;
  }
  {
    if (subn == 0 || subn > 19) {
      return 2;
    }
    int64_t v = 0;
    for (uint32_t i = 0; i < subn; i++) {
      if (!isdigit((uint8_t)subs[i])) {
        return 2;
      }
      int d = subs[i] - '0';
      if (v > (INT64_MAX - d) / 10) {
        return 2;
      }
      v = v * 10 + d;
    }
    if (v <= 0) {
      return 2;
    }
    int un = junesc(uname, ucap, names, namen);
    if (un < 0) {
      return 2;
    }
    *ulen = (uint16_t)un;
    *uid = v;
    rc = 0;
  }
  return rc;
}

// Authorization header value -> uid/username or error
// returns 0 ok / 1 missing bearer / 2 invalid or expired / 3 invalid payload
int auth_check(const char* v, size_t n, int64_t* uid, char* uname,
               size_t ucap, uint16_t* ulen) {
  if (!v) return 1;
  while (n && (*v == ' ' || *v == '\t')) {
    v++;
    n--;
  }
  if (n < 8 || memcmp(v, "Bearer ", 7)) return 1;
  int rc = jwt_check(v + 7, n - 7, uid, uname, ucap, ulen);
  return rc == 0 ? 0 : rc == 2 ? 3 : 2;
}

// ------------------------------------------------------------------ exports for pgw scram

void sha256_oneshot(const void* data, size_t n, uint8_t out[32]) {
  Sha256 s;
  sha_init(&s);
  sha_upd(&s, data, n);
  sha_fin(&s, out);
}

int b64_enc(const uint8_t* s, size_t n, char* out) {
  static const char A[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0, i = 0;
  for (; i + 3 <= n; i += 3) {
    uint32_t v = s[i] << 16 | s[i + 1] << 8 | s[i + 2];
    out[o] = A[v >> 18];
    out[o + 1] = A[v >> 12 & 63];
    out[o + 2] = A[v >> 6 & 63];
    out[o + 3] = A[v & 63];
    o += 4;
  }
  if (n - i == 1) {
    out[o] = A[s[i] >> 2];
    out[o + 1] = A[s[i] << 4 & 63];
    out[o + 2] = '=';
    out[o + 3] = '=';
    o += 4;
  } else if (n - i == 2) {
    uint32_t v = s[i] << 16 | s[i + 1] << 8;
    out[o] = A[v >> 18];
    out[o + 1] = A[v >> 12 & 63];
    out[o + 2] = A[v >> 6 & 63];
    out[o + 3] = '=';
    o += 4;
  }
  return (int)o;
}
