// JSON and UTF-8 for route A′'s host: a strict JSON parser into a small
// tree (ECMA-404, what JSON.parse accepts), the UTF-8 repair JavaScript's
// TextDecoder does, JavaScript's String.prototype.trim, and JSON string
// escaping as JSON.stringify writes it. Used by host/http.h (request bodies,
// JWT parts) and host/pg.c (the golden files of --db canned). Included by
// both C files of one translation unit, so it has an include guard and only
// static definitions. Outside every proof.
#ifndef BM_JSON_H
#define BM_JSON_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ---- a growable byte buffer ----

typedef struct {
  char*  p;
  size_t n;
  size_t cap;
} JBuf;

// The appends are inlined and only the growth is a call: a reply is built from a hundred small
// appends, most of them of a literal, whose length is then a constant and whose copy a few
// stores (issue #67: jb_need and jb_put held 780 of route A′'s 23,203 user-space cycles per
// request as calls).
__attribute__((noinline, cold)) static void jb_grow(JBuf* b, size_t more) {
  size_t cap = b->cap ? b->cap : 256;
  while (cap < b->n + more) {
    cap *= 2;
  }
  char* p = realloc(b->p, cap);
  if (p == NULL) {
    abort();
  }
  b->p   = p;
  b->cap = cap;
}

__attribute__((always_inline)) static inline void jb_need(JBuf* b, size_t more) {
  if (__builtin_expect(b->n + more > b->cap, 0)) {
    jb_grow(b, more);
  }
}

__attribute__((always_inline)) static inline void jb_put(JBuf* b, const void* p, size_t n) {
  jb_need(b, n);
  memcpy(b->p + b->n, p, n);
  b->n += n;
}

__attribute__((always_inline)) static inline void jb_byte(JBuf* b, char c) {
  jb_need(b, 1);
  b->p[b->n++] = c;
}

__attribute__((always_inline)) static inline void jb_str(JBuf* b, const char* s) {
  jb_put(b, s, strlen(s));
}

// The digit pairs "00" to "99".
static const char jb_dd[201] =
  "00010203040506070809" "10111213141516171819" "20212223242526272829" "30313233343536373839"
  "40414243444546474849" "50515253545556575859" "60616263646566676869" "70717273747576777879"
  "80818283848586878889" "90919293949596979899";

// v in decimal: two digits a step, right-aligned to end at t[24], then 24 bytes copied from its
// first digit (a fixed size, so no call to memcpy; the bytes after the digits are t's zeros
// and lie past n, where the next append writes) and n moved by the digits alone.
static void jb_u64(JBuf* b, uint64_t v) {
  char t[48] = { 0 };
  int  i = 24;
  while (v >= 100) {
    uint64_t q = v / 100;
    i -= 2;
    memcpy(t + i, jb_dd + 2 * (v - q * 100), 2);
    v = q;
  }
  if (v >= 10) {
    i -= 2;
    memcpy(t + i, jb_dd + 2 * v, 2);
  } else {
    t[--i] = (char)('0' + v);
  }
  jb_need(b, 24);
  memcpy(b->p + b->n, t + i, 24);
  b->n += (size_t)(24 - i);
}

// ---- UTF-8 ----

static size_t utf8_put(char* o, uint32_t c) {
  if (c < 0x80) {
    o[0] = (char)c;
    return 1;
  }
  if (c < 0x800) {
    o[0] = (char)(0xC0 | (c >> 6));
    o[1] = (char)(0x80 | (c & 0x3F));
    return 2;
  }
  if (c < 0x10000) {
    o[0] = (char)(0xE0 | (c >> 12));
    o[1] = (char)(0x80 | ((c >> 6) & 0x3F));
    o[2] = (char)(0x80 | (c & 0x3F));
    return 3;
  }
  o[0] = (char)(0xF0 | (c >> 18));
  o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
  o[2] = (char)(0x80 | ((c >> 6) & 0x3F));
  o[3] = (char)(0x80 | (c & 0x3F));
  return 4;
}

// One code point from p[*i ..< n]; an ill-formed sequence is one U+FFFD per
// maximal subpart (the WHATWG decoder, as TextDecoder and io_str do).
static uint32_t utf8_next(const unsigned char* p, size_t n, size_t* i) {
  unsigned b = p[(*i)++];
  if (b < 0x80) {
    return b;
  }
  if (b < 0xC2 || b > 0xF4) {
    return 0xFFFD;
  }
  unsigned need = b < 0xE0 ? 1 : b < 0xF0 ? 2 : 3;
  unsigned lo   = b == 0xE0 ? 0xA0 : b == 0xF0 ? 0x90 : 0x80;
  unsigned hi   = b == 0xED ? 0x9F : b == 0xF4 ? 0x8F : 0xBF;
  uint32_t c    = b & (0x3F >> need);
  while (need > 0) {
    if (*i >= n || p[*i] < lo || p[*i] > hi) {
      return 0xFFFD;
    }
    c  = (c << 6) | (p[(*i)++] & 0x3F);
    lo = 0x80;
    hi = 0xBF;
    need -= 1;
  }
  return c;
}

// p[0 ..< n] as well-formed UTF-8 into out (replaced, not appended to): a
// leading byte-order mark dropped, every ill-formed sequence U+FFFD.
// TextDecoder's default, which Request.text() uses.
static void utf8_repair(const char* p, size_t n, JBuf* out) {
  const unsigned char* u = (const unsigned char*)p;
  size_t i = n >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF ? 3 : 0;
  out->n = 0;
  jb_need(out, n * 3 + 1);
  while (i < n) {
    if (u[i] < 0x80) {
      out->p[out->n++] = (char)u[i++];
      continue;
    }
    out->n += utf8_put(out->p + out->n, utf8_next(u, n, &i));
  }
}

// String.prototype.trim's set: WhiteSpace and LineTerminator (ECMA-262).
static bool js_space(uint32_t c) {
  return c == 0x09 || c == 0x0A || c == 0x0B || c == 0x0C || c == 0x0D || c == 0x20
    || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028
    || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

// The trimmed slice of well-formed UTF-8 p[0 ..< n]: *start and *len, and
// its length in code points.
static size_t js_trim(const char* p, size_t n, size_t* start, size_t* len) {
  const unsigned char* u = (const unsigned char*)p;
  size_t i = 0, first = n, last = 0, count = 0, cfirst = 0, clast = 0;
  while (i < n) {
    size_t   at = i;
    uint32_t c  = utf8_next(u, n, &i);
    count += 1;
    if (!js_space(c)) {
      if (first == n) {
        first  = at;
        cfirst = count - 1;
      }
      last  = i;
      clast = count;
    }
  }
  if (first == n) {
    *start = 0;
    *len   = 0;
    return 0;
  }
  *start = first;
  *len   = last - first;
  return clast - cfirst;
}

// JSON.stringify's escaping of a well-formed UTF-8 string, quotes included.
static void json_quote(JBuf* b, const char* p, size_t n) {
  static const char hex[] = "0123456789abcdef";
  jb_need(b, n + 2);
  b->p[b->n++] = '"';
  for (size_t i = 0; i < n; i += 1) {
    unsigned char c = (unsigned char)p[i];
    if (c >= 0x20 && c != '"' && c != '\\') {
      jb_byte(b, (char)c);
      continue;
    }
    jb_byte(b, '\\');
    switch (c) {
    case '"': jb_byte(b, '"'); break;
    case '\\': jb_byte(b, '\\'); break;
    case '\b': jb_byte(b, 'b'); break;
    case '\f': jb_byte(b, 'f'); break;
    case '\n': jb_byte(b, 'n'); break;
    case '\r': jb_byte(b, 'r'); break;
    case '\t': jb_byte(b, 't'); break;
    default:
      jb_str(b, "u00");
      jb_byte(b, hex[c >> 4]);
      jb_byte(b, hex[c & 15]);
    }
  }
  jb_byte(b, '"');
}

// ---- the parser ----

enum { J_NULL, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ };

// A node: its kind; for J_STR its decoded bytes at text[s ..< s + n]; for
// J_NUM its value; for J_ARR and J_OBJ its first child; next is the next
// sibling. An object's children are its values, each with its key at
// text[k ..< k + kn]. Indices, not pointers: both arrays grow.
typedef struct {
  uint8_t  kind;
  uint32_t s, n, k, kn;
  int32_t  child, next;
  double   num;
} JNode;

typedef struct {
  JNode*      v;
  uint32_t    n, cap;
  JBuf        text;
  const char* p;
  size_t      at, len;
  uint32_t    depth;
  bool        nul;  // a string held U+0000
} JDoc;

#define J_DEPTH 256

static int32_t j_node(JDoc* d, uint8_t kind) {
  if (d->n == d->cap) {
    uint32_t cap = d->cap ? d->cap * 2 : 64;
    JNode*   v   = realloc(d->v, cap * sizeof *v);
    if (v == NULL) {
      abort();
    }
    d->v   = v;
    d->cap = cap;
  }
  d->v[d->n] = (JNode){ .kind = kind, .child = -1, .next = -1 };
  return (int32_t)d->n++;
}

static void j_ws(JDoc* d) {
  while (d->at < d->len) {
    char c = d->p[d->at];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
      return;
    }
    d->at += 1;
  }
}

static int j_hex4(JDoc* d, uint32_t* out) {
  if (d->at + 4 > d->len) {
    return -1;
  }
  uint32_t v = 0;
  for (int i = 0; i < 4; i += 1) {
    char c = d->p[d->at++];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
    else return -1;
  }
  *out = v;
  return 0;
}

// A string at d->at (on its opening quote), decoded into d->text; a lone
// surrogate escape becomes U+FFFD (what TextEncoder writes for it).
static int j_string(JDoc* d, uint32_t* s, uint32_t* n) {
  d->at += 1;
  *s = (uint32_t)d->text.n;
  for (;;) {
    if (d->at >= d->len) {
      return -1;
    }
    unsigned char c = (unsigned char)d->p[d->at];
    if (c == '"') {
      d->at += 1;
      *n = (uint32_t)(d->text.n - *s);
      return 0;
    }
    if (c < 0x20) {
      return -1;
    }
    if (c != '\\') {
      size_t run = d->at;
      while (run < d->len) {
        unsigned char r = (unsigned char)d->p[run];
        if (r == '"' || r == '\\' || r < 0x20) {
          break;
        }
        run += 1;
      }
      jb_put(&d->text, d->p + d->at, run - d->at);
      d->at = run;
      continue;
    }
    d->at += 1;
    if (d->at >= d->len) {
      return -1;
    }
    char e = d->p[d->at++];
    switch (e) {
    case '"': jb_byte(&d->text, '"'); break;
    case '\\': jb_byte(&d->text, '\\'); break;
    case '/': jb_byte(&d->text, '/'); break;
    case 'b': jb_byte(&d->text, '\b'); break;
    case 'f': jb_byte(&d->text, '\f'); break;
    case 'n': jb_byte(&d->text, '\n'); break;
    case 'r': jb_byte(&d->text, '\r'); break;
    case 't': jb_byte(&d->text, '\t'); break;
    case 'u': {
      uint32_t u;
      if (j_hex4(d, &u) < 0) {
        return -1;
      }
      if (u >= 0xD800 && u <= 0xDBFF && d->at + 6 <= d->len && d->p[d->at] == '\\'
        && d->p[d->at + 1] == 'u') {
        size_t   back = d->at;
        uint32_t lo;
        d->at += 2;
        if (j_hex4(d, &lo) < 0) {
          return -1;
        }
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
        } else {
          d->at = back;  // the next escape stands on its own
          u     = 0xFFFD;
        }
      } else if (u >= 0xD800 && u <= 0xDFFF) {
        u = 0xFFFD;
      }
      if (u == 0) {
        d->nul = true;
      }
      char t[4];
      jb_put(&d->text, t, utf8_put(t, u));
      break;
    }
    default:
      return -1;
    }
  }
}

static int j_number(JDoc* d, double* out) {
  size_t a = d->at;
  if (d->at < d->len && d->p[d->at] == '-') d->at += 1;
  if (d->at >= d->len) return -1;
  if (d->p[d->at] == '0') {
    d->at += 1;
  } else if (d->p[d->at] >= '1' && d->p[d->at] <= '9') {
    while (d->at < d->len && d->p[d->at] >= '0' && d->p[d->at] <= '9') d->at += 1;
  } else {
    return -1;
  }
  if (d->at < d->len && d->p[d->at] == '.') {
    d->at += 1;
    if (d->at >= d->len || d->p[d->at] < '0' || d->p[d->at] > '9') return -1;
    while (d->at < d->len && d->p[d->at] >= '0' && d->p[d->at] <= '9') d->at += 1;
  }
  if (d->at < d->len && (d->p[d->at] == 'e' || d->p[d->at] == 'E')) {
    d->at += 1;
    if (d->at < d->len && (d->p[d->at] == '+' || d->p[d->at] == '-')) d->at += 1;
    if (d->at >= d->len || d->p[d->at] < '0' || d->p[d->at] > '9') return -1;
    while (d->at < d->len && d->p[d->at] >= '0' && d->p[d->at] <= '9') d->at += 1;
  }
  char  t[64];
  size_t n = d->at - a;
  if (n >= sizeof t) {
    char* big = malloc(n + 1);
    if (big == NULL) abort();
    memcpy(big, d->p + a, n);
    big[n] = 0;
    *out   = strtod(big, NULL);
    free(big);
  } else {
    memcpy(t, d->p + a, n);
    t[n] = 0;
    *out = strtod(t, NULL);
  }
  return 0;
}

static bool j_lit(JDoc* d, const char* w) {
  size_t n = strlen(w);
  if (d->at + n > d->len || memcmp(d->p + d->at, w, n) != 0) {
    return false;
  }
  d->at += n;
  return true;
}

static int32_t j_value(JDoc* d);

static int32_t j_members(JDoc* d, int32_t node, bool obj) {
  d->at += 1;
  if (++d->depth > J_DEPTH) {
    return -1;
  }
  j_ws(d);
  int32_t last = -1;
  if (d->at < d->len && d->p[d->at] == (obj ? '}' : ']')) {
    d->at += 1;
    d->depth -= 1;
    return node;
  }
  for (;;) {
    uint32_t k = 0, kn = 0;
    if (obj) {
      j_ws(d);
      if (d->at >= d->len || d->p[d->at] != '"' || j_string(d, &k, &kn) < 0) {
        return -1;
      }
      j_ws(d);
      if (d->at >= d->len || d->p[d->at] != ':') {
        return -1;
      }
      d->at += 1;
    }
    int32_t v = j_value(d);
    if (v < 0) {
      return -1;
    }
    d->v[v].k  = k;
    d->v[v].kn = kn;
    if (last < 0) {
      d->v[node].child = v;
    } else {
      d->v[last].next = v;
    }
    last = v;
    d->v[node].n += 1;
    j_ws(d);
    if (d->at >= d->len) {
      return -1;
    }
    char c = d->p[d->at++];
    if (c == ',') {
      continue;
    }
    if (c == (obj ? '}' : ']')) {
      d->depth -= 1;
      return node;
    }
    return -1;
  }
}

static int32_t j_value(JDoc* d) {
  j_ws(d);
  if (d->at >= d->len) {
    return -1;
  }
  char c = d->p[d->at];
  if (c == '{') return j_members(d, j_node(d, J_OBJ), true);
  if (c == '[') return j_members(d, j_node(d, J_ARR), false);
  if (c == '"') {
    int32_t x = j_node(d, J_STR);
    uint32_t s, n;
    if (j_string(d, &s, &n) < 0) return -1;
    d->v[x].s = s;
    d->v[x].n = n;
    return x;
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    int32_t x = j_node(d, J_NUM);
    double  v;
    if (j_number(d, &v) < 0) return -1;
    d->v[x].num = v;
    return x;
  }
  if (j_lit(d, "true")) return j_node(d, J_TRUE);
  if (j_lit(d, "false")) return j_node(d, J_FALSE);
  if (j_lit(d, "null")) return j_node(d, J_NULL);
  return -1;
}

// Parses well-formed UTF-8 p[0 ..< n] into d (reused across calls): the root
// node, or -1 when it is not exactly one JSON value between optional
// whitespace.
static int32_t json_parse(JDoc* d, const char* p, size_t n) {
  d->n      = 0;
  d->text.n = 0;
  d->p      = p;
  d->at     = 0;
  d->len    = n;
  d->depth  = 0;
  d->nul    = false;
  int32_t r = j_value(d);
  if (r < 0) {
    return -1;
  }
  j_ws(d);
  return d->at == d->len ? r : -1;
}

// The value of key in object node o: the last member with that key, as
// JSON.parse keeps it; -1 when there is none or o is not an object.
static int32_t json_get(const JDoc* d, int32_t o, const char* key) {
  if (o < 0 || d->v[o].kind != J_OBJ) {
    return -1;
  }
  size_t  kn  = strlen(key);
  int32_t hit = -1;
  for (int32_t c = d->v[o].child; c >= 0; c = d->v[c].next) {
    if (d->v[c].kn == kn && memcmp(d->text.p + d->v[c].k, key, kn) == 0) {
      hit = c;
    }
  }
  return hit;
}

static const char* json_text(const JDoc* d, int32_t x) {
  return d->text.p + d->v[x].s;
}

#endif
