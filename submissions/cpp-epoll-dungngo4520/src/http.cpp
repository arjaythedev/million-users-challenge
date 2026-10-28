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
  if (n == 0 || n > 19) return false;
  int64_t v = 0;
  for (size_t i = 0; i < n; i++) {
    if (!isdigit((uint8_t)s[i])) return false;
    int d = s[i] - '0';
    if (v > (INT64_MAX - d) / 10) return false;
    v = v * 10 + d;
  }
  if (v <= 0) return false;
  *out = v;
  return true;
}

// returns bytes consumed by one request, or 0 if incomplete / -1 bad
long http_parse(const char* s, size_t n, int* method, const char** path,
                size_t* plen, const char** auth, size_t* authn,
                const char** body, size_t* blen, int* minor,
                bool* connclose, bool* gz) {
  const char* he = (const char*)memmem(s, n, "\r\n\r\n", 4);
  if (!he) return 0;
  size_t hdr = he + 4 - s;

  const char* sp = (const char*)memchr(s, ' ', hdr);
  if (!sp) return -1;
  size_t ml = sp - s;
  if (ml == 3 && !memcmp(s, "GET", 3)) *method = M_GET;
  else if (ml == 4 && !memcmp(s, "POST", 4)) *method = M_POST;
  else *method = M_OTHER;
  const char* sp2 = (const char*)memchr(sp + 1, ' ', hdr);
  if (!sp2) return -1;
  *path = sp + 1;
  *plen = sp2 - sp - 1;
  *minor = sp2 + 8 < he && sp2[8] == '0' ? 0 : 1;  // HTTP/1.x

  *auth = nullptr;
  *authn = 0;
  *gz = false;
  *connclose = *minor == 0;  // HTTP/1.0 closes unless keep-alive
  long long cl = 0;
  const char* p = (const char*)memchr(sp2, '\n', hdr - (sp2 - s));
  if (!p) return -1;
  p++;
  // header lines occupy [p, he); the last one ends AT he (its \r\n is part of
  // the "\r\n\r\n" terminator), so it has no trailing CRLF inside the range.
  while (p < he) {
    const char* e = (const char*)memmem(p, he - p, "\r\n", 2);
    size_t ll = e ? (size_t)(e - p) : (size_t)(he - p);
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
      if (kn == 13 && !strncasecmp(p, "authorization", 13)) {
        *auth = v;
        *authn = vn;
      } else if (kn == 15 && !strncasecmp(p, "accept-encoding", 15)) {
        for (size_t k = 0; k + 4 <= vn; k++)
          if (!strncasecmp(v + k, "gzip", 4)) {
            *gz = true;
            break;
          }
      } else if (kn == 14 && !strncasecmp(p, "content-length", 14)) {
        cl = strtoll(v, nullptr, 10);
        if (cl < 0) cl = 0;
      } else if (kn == 10 && !strncasecmp(p, "connection", 10)) {
        if (vn == 5 && !strncasecmp(v, "close", 5)) *connclose = true;
        if (vn == 10 && !strncasecmp(v, "keep-alive", 10)) *connclose = false;
      }
    }
    p = e ? e + 2 : he;
  }
  if ((size_t)(n - hdr) < (size_t)cl) return 0;  // body still arriving
  if (cl > (1 << 20)) return -1;
  *body = he + 4;
  *blen = cl;
  return (long)(hdr + cl);
}
