// Request lifecycle and routing: request objects are recycled per worker,
// routed in dispatch(), and completed either inline (errors) or by
// req_pg_done() when the database answers. Responses are built into the
// request's own inline area (header written in front of the serialized
// body), so the hot path allocates nothing.

#include "server.h"

#include <libdeflate.h>
#ifdef HAVE_ISAL
#include <isa-l.h>
#endif

// ------------------------------------------------------------------ gzip

// one-shot gzip into dst; 0 = doesn't fit (caller sends uncompressed).
// igzip level 0 is ~2.3x faster than libdeflate level 1 at response
// sizes, for ~36% larger output - a good trade while compression is
// optional and the wire is cheap.
static size_t gz_one(Worker* w, const char* src, size_t n, char* dst,
                     size_t cap) {
#ifdef HAVE_ISAL
  (void)w;
  static thread_local struct isal_zstream zs;
  isal_deflate_stateless_init(&zs);
  zs.level = 0;
  zs.gzip_flag = IGZIP_GZIP;
  // static tables skip the per-call histogram + huffman build: at ~5KB
  // inputs the dynamic table costs nearly what the encode does.
  // IGZIP_DEFLATE would be CUSTOM here - nullptr makes it a silent no-op
  if (isal_deflate_set_hufftables(&zs, nullptr, IGZIP_HUFFTABLE_STATIC) !=
      COMP_OK)
    return 0;  // caller falls back to an uncompressed response
  zs.next_in = (uint8_t*)src;
  zs.avail_in = (uint32_t)n;
  zs.next_out = (uint8_t*)dst;
  zs.avail_out = (uint32_t)cap;
  zs.end_of_stream = 1;
  zs.flush = NO_FLUSH;
  if (isal_deflate_stateless(&zs) != COMP_OK) return 0;
  return zs.total_out;
#else
  if (!w->gzc) w->gzc = libdeflate_alloc_compressor(1);
  if (!w->gzc) return 0;
  return libdeflate_gzip_compress((libdeflate_compressor*)w->gzc, src, n,
                                  dst, cap);
#endif
}

// ------------------------------------------------------------------ req lifecycle

Req* req_new(Worker* w, Conn* c) {
  Req* r;
  if (LIKELY_P(!w->rfree.empty(), 0.9)) {
    r = w->rfree.back();
    w->rfree.pop_back();
  } else if (w->rused < (512u << 20) / sizeof(Req)) {
    // bump-carve from the per-worker THP arena: fresher than malloc for
    // locality and never returns a freelist-recycled line to the heap.
    // No zeroing needed - freelist reuse already hands out dirty Reqs, so
    // every field req_new leaves alone is write-before-read downstream.
    r = (Req*)(w->rarena + w->rused * sizeof(Req));
    w->rused++;
  } else {
    r = (Req*)calloc(1, sizeof(Req));  // overflow beyond the arena design
    if (UNLIKELY(!r)) die("oom");
  }
  r->w = w;
  r->c = c;
  r->c_next = nullptr;
  r->pg_next = nullptr;
  r->resp = nullptr;
  r->rlen = 0;
  r->done = 0;
  r->on_pg = 0;
  r->resp_kind = RK_NONE;
  r->gz = 0;
  r->held = 0;
  if (c->rq_tail) c->rq_tail->c_next = r;
  else c->rq_head = r;
  c->rq_tail = r;
  w->batch_reqs++;
  return r;
}

void req_free(Worker* w, Req* r) {
  if (UNLIKELY(r->resp_kind == RK_HEAP)) free(r->resp);
  r->resp = nullptr;
  r->resp_kind = RK_NONE;
  r->held = 0;
  // cap the freelist: a PG stall can park thousands of 19KB Reqs; beyond
  // a working-set cap they are cheaper freed than hoarded. Arena-carved
  // Reqs are not malloc'd - dropping them just loses bump space, so only
  // the calloc overflow path actually reaches free().
  if (UNLIKELY(w->rfree.size() >= 1024)) {
    if (r < w->rarena || r >= w->rarena + (512u << 20)) free(r);
    return;
  }
  w->rfree.push_back(r);
}

// "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n[Content-Encoding: gzip\r\n]Content-Length: n\r\n\r\n"
static char* hdr_build(char* d, int status, size_t olen, bool gz) {
  memcpy(d, "HTTP/1.1 ", 9);
  d += 9;
  d[0] = (char)('0' + status / 100);
  d[1] = (char)('0' + status / 10 % 10);
  d[2] = (char)('0' + status % 10);
  d += 3;
  *d++ = ' ';
  const char* rsn;
  size_t rn;
  switch (status) {
    case 200: rsn = "OK"; rn = 2; break;
    case 201: rsn = "Created"; rn = 7; break;
    case 400: rsn = "Bad Request"; rn = 11; break;
    case 401: rsn = "Unauthorized"; rn = 12; break;
    case 404: rsn = "Not Found"; rn = 9; break;
    case 500: rsn = "Internal Server Error"; rn = 21; break;
    case 503: rsn = "Service Unavailable"; rn = 19; break;
    default: rsn = "OK"; rn = 2; break;
  }
  memcpy(d, rsn, rn);
  d += rn;
  static const char CT[] = "\r\nContent-Type: application/json\r\n";
  memcpy(d, CT, sizeof(CT) - 1);
  d += sizeof(CT) - 1;
  if (gz) {
    static const char CE[] = "Content-Encoding: gzip\r\n";
    memcpy(d, CE, sizeof(CE) - 1);
    d += sizeof(CE) - 1;
  }
  static const char CL[] = "Content-Length: ";
  memcpy(d, CL, sizeof(CL) - 1);
  d += sizeof(CL) - 1;
  char nb[24];
  char* ne = nb + sizeof(nb);
  char* ns = u64toa(olen, ne);
  memcpy(d, ns, (size_t)(ne - ns));
  d += ne - ns;
  memcpy(d, "\r\n\r\n", 4);
  return d + 4;
}

// build full response (header+body) into r->resp, mark done. Bodies that
// fit go to rarea with the header prepended in place; gzip when the client
// accepts it and the compressed stream fits too.
void req_resp(Req* r, int status, const char* body, size_t blen) {
  Worker* w = r->w;
  const char* out = body;
  size_t olen = blen;
  bool gzout = false;
  if (r->gz && blen >= 1024 && body != r->rarea + RESP_PAD) {
    // compress straight into the response area: a compressor that can't
    // beat the buffer just means "send uncompressed" - no worst-case
    // bound check, so big bodies still get compressed
    size_t gn = gz_one(w, body, blen, r->rarea + RESP_PAD, RESP_CAP);
    if (LIKELY(gn)) {
      out = r->rarea + RESP_PAD;
      olen = gn;
      gzout = true;
    }
  }
  if (!gzout && blen <= RESP_CAP) {
    if (LIKELY(body != r->rarea + RESP_PAD)) memcpy(r->rarea + RESP_PAD, body, blen);
    out = r->rarea + RESP_PAD;
  }
  char hb[RESP_PAD];
  char* he = hdr_build(hb, status, olen, gzout);
  size_t hn = (size_t)(he - hb);
  if (LIKELY_P(out == r->rarea + RESP_PAD, 0.9)) {
    memcpy(r->rarea + RESP_PAD - hn, hb, hn);
    r->resp = r->rarea + RESP_PAD - hn;
    r->rlen = (uint32_t)(hn + olen);
    r->resp_kind = RK_INLINE;
  } else {
    r->resp = (char*)malloc(hn + olen);
    memcpy(r->resp, hb, hn);
    memcpy(r->resp + hn, out, olen);
    r->rlen = (uint32_t)(hn + olen);
    r->resp_kind = RK_HEAP;
  }
  r->status = status;
  r->done = 1;
}

// complete responses for every spec-mandated error body, built once.
// errors are <1% of traffic: keep this out of the hot sections.
COLD void req_err(Req* r, int status, const char* msg) {
  static const struct {
    int st;
    const char* msg;
  } ERRS[] = {
      {400, "invalid post id"},
      {400, "malformed JSON body"},
      {400, "body is required"},
      {400, "body must be at most 500 characters"},
      {401, "missing bearer token"},
      {401, "invalid or expired token"},
      {401, "invalid token payload"},
      {404, "not found"},
      {404, "post not found"},
      {500, "internal server error"},
  };
  static const struct Blobs {
    const char* s[sizeof(ERRS) / sizeof(ERRS[0])];
    size_t n[sizeof(ERRS) / sizeof(ERRS[0])];
  } blobs = [] {
    static char buf[2048];
    Blobs b{};
    char* p = buf;
    for (size_t i = 0; i < sizeof(ERRS) / sizeof(ERRS[0]); i++) {
      char jb[160];
      int jn = snprintf(jb, sizeof(jb), "{\"error\":\"%s\"}", ERRS[i].msg);
      char* he = hdr_build(p, ERRS[i].st, (size_t)jn, false);
      memcpy(he, jb, (size_t)jn);
      b.s[i] = p;
      b.n[i] = (size_t)(he - p) + (size_t)jn;
      p += b.n[i];
    }
    return b;
  }();
  for (size_t i = 0; i < sizeof(ERRS) / sizeof(ERRS[0]); i++)
    if (ERRS[i].st == status && !strcmp(ERRS[i].msg, msg)) {
      // an earlier response (e.g. a rolled-back write's 'C' result) may
      // already own heap memory - drop it before rebinding to the blob
      if (r->resp_kind == RK_HEAP) free(r->resp);
      r->resp = (char*)blobs.s[i];
      r->rlen = (uint32_t)blobs.n[i];
      r->resp_kind = RK_CONST;
      r->status = status;
      r->done = 1;
      return;
    }
  char b[160];
  int n = snprintf(b, sizeof(b), "{\"error\":\"%s\"}", msg);
  req_resp(r, status, b, (size_t)n);
}

// "2025-12-31 23:59:19.409123+00" -> "2025-12-31T23:59:19.409Z"
// (connections run with SET TIME ZONE 'UTC'; ISO DateStyle)
static void put_ts(Buf& b, const char* s, int slen) {
  if (slen == 8) {
    // binary timestamptz: int64 usec since 2000-01-01 (the Bind asked for
    // it; ~20 bytes saved per row vs text). Emit identical ISO bytes.
    int64_t v;
    memcpy(&v, s, 8);
    v = __builtin_bswap64(v) + 946684800000000LL;  // -> unix usec
    if (UNLIKELY(v < 0)) return;
    int64_t days = v / 86400000000LL;
    uint64_t rem = (uint64_t)(v % 86400000000LL);
    // the civil date only changes at day boundaries: cache the last one
    // (a feed's 20 rows sit within ~minutes of each other)
    static thread_local int64_t cd = -1;
    static thread_local int cy, cm, cdoy;
    if (UNLIKELY(days != cd)) {
      // civil-from-days (Hinnant)
      int64_t z = days + 719468;
      int64_t era = (z >= 0 ? z : z - 146096) / 146097;
      unsigned doe = (unsigned)(z - era * 146097);
      unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
      int64_t y = (int64_t)yoe + era * 400;
      unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
      unsigned mp = (5 * doy + 2) / 153;
      cd = days;
      cy = (int)y;
      cm = (int)(mp < 10 ? mp + 3 : mp - 9);
      cdoy = (int)(doy - (153 * mp + 2) / 5 + 1);
      if (cm <= 2) cy++;
    }
    char o[24];
    char* p = o;
    *p++ = (char)('0' + cy / 1000 % 10);
    *p++ = (char)('0' + cy / 100 % 10);
    *p++ = (char)('0' + cy / 10 % 10);
    *p++ = (char)('0' + cy % 10);
    *p++ = '-';
    *p++ = (char)('0' + cm / 10);
    *p++ = (char)('0' + cm % 10);
    *p++ = '-';
    *p++ = (char)('0' + cdoy / 10);
    *p++ = (char)('0' + cdoy % 10);
    *p++ = 'T';
    uint64_t hh = rem / 3600000000u, mm = rem / 60000000u % 60,
             ss = rem / 1000000u % 60, ms = rem / 1000u % 1000;
    *p++ = (char)('0' + hh / 10);
    *p++ = (char)('0' + hh % 10);
    *p++ = ':';
    *p++ = (char)('0' + mm / 10);
    *p++ = (char)('0' + mm % 10);
    *p++ = ':';
    *p++ = (char)('0' + ss / 10);
    *p++ = (char)('0' + ss % 10);
    *p++ = '.';
    *p++ = (char)('0' + ms / 100);
    *p++ = (char)('0' + ms / 10 % 10);
    *p++ = (char)('0' + ms % 10);
    *p++ = 'Z';
    b.put(o, (size_t)(p - o));
    return;
  }
  // date part YYYY-MM-DD
  if (UNLIKELY(slen < 19)) {
    b.put(s, (size_t)slen);
    return;
  }
  b.put(s, 10);
  b.ch('T');
  b.put(s + 11, 8);
  b.ch('.');
  if (LIKELY(s[19] == '.')) {
    int i = 0;
    while (i < 3 && isdigit((uint8_t)s[20 + i])) {
      b.ch(s[20 + i]);
      i++;
    }
    while (i < 3) {
      b.ch('0');
      i++;
    }
  } else {
    b.puts("000");
  }
  b.ch('Z');
}

// finish a request that got a PG result
void req_pg_done(Worker* w, Req* r, PgRes* res) {
  Conn* c = r->c;
  r->on_pg = 0;
  if (UNLIKELY(!c)) {
    req_free(w, r);
    return;
  }
  static thread_local char scratch[65536];
  // non-gz bodies are serialized in place inside the response area; gz
  // bodies go through scratch because libdeflate cannot work in place
  const char* body;
  size_t blen = 0;
  int st = 500;
  auto build = [&](Buf& b) {
    int s = 200;
    int est = res->status;
    auto gv = [&](int i, int j) {
      return res->base + res->rows[i * res->ncols + j].off;
    };
    auto gl = [&](int i, int j) { return res->rows[i * res->ncols + j].l; };
  switch (r->type) {
    case R_HEALTH:
      if (LIKELY(est == PGR_TUPLES)) {
        b.puts("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
        char nb[24];
        char* ne = nb + sizeof(nb);
        char* ns = u64toa(
            (uint64_t)((time_t)g_now_s.load(std::memory_order_relaxed) -
                       g_start),
            ne);
        b.put(ns, (size_t)(ne - ns));
        b.ch('}');
      } else {
        s = 503;
        b.puts("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\""
               "db query failed\"}");
      }
      break;
    case R_FEED: {
      if (UNLIKELY(est != PGR_TUPLES)) {
        s = 500;
        b.puts("{\"error\":\"internal server error\"}");
        break;
      }
      int rows = res->nrows;
      b.puts("{\"posts\":[");
      for (int i = 0; i < rows; i++) {
        if (i) b.ch(',');
        b.puts("{\"id\":");
        b.put(gv(i, 0), (size_t)gl(i, 0));
        b.puts(",\"body\":\"");
        b.jesc(gv(i, 1), gl(i, 1));
        b.puts("\",\"created_at\":\"");
        put_ts(b, gv(i, 2), gl(i, 2));
        b.puts("\",\"author\":\"");
        b.jesc(gv(i, 3), gl(i, 3));
        b.puts("\",\"like_count\":");
        b.put(gv(i, 4), (size_t)gl(i, 4));
        b.ch('}');
      }
      b.puts("]}");
      break;
    }
    case R_GET: {
      if (UNLIKELY(est != PGR_TUPLES)) {
        s = 500;
        b.puts("{\"error\":\"internal server error\"}");
        break;
      }
      if (UNLIKELY(res->nrows == 0)) {
        s = 404;
        b.puts("{\"error\":\"post not found\"}");
        break;
      }
      b.puts("{\"post\":{\"id\":");
      b.put(gv(0, 0), (size_t)gl(0, 0));
      b.puts(",\"body\":\"");
      b.jesc(gv(0, 1), gl(0, 1));
      b.puts("\",\"created_at\":\"");
      put_ts(b, gv(0, 2), gl(0, 2));
      b.puts("\",\"author\":\"");
      b.jesc(gv(0, 3), gl(0, 3));
      b.puts("\",\"like_count\":");
      b.put(gv(0, 4), (size_t)gl(0, 4));
      b.puts("}}");
      break;
    }
    case R_CREATE: {
      if (LIKELY(est == PGR_TUPLES && res->nrows == 1)) {
        s = 201;
        b.puts("{\"post\":{\"id\":");
        b.put(gv(0, 0), (size_t)gl(0, 0));
        b.puts(",\"body\":\"");
        b.jesc(r->body, r->blen);
        b.puts("\",\"created_at\":\"");
        put_ts(b, gv(0, 1), gl(0, 1));
        b.puts("\",\"author\":\"");
        b.jesc(r->uname, r->ulen);
        b.puts("\",\"like_count\":0}}");
      } else {
        s = 500;
        b.puts("{\"error\":\"internal server error\"}");
      }
      break;
    }
    case R_LIKE: {
      if (LIKELY(est == PGR_COMMAND)) {
        // "INSERT 0 N": the affected row count is the tag's last field
        const char* aff = res->tag ? res->tag + res->taglen : "";
        bool ins = res->taglen > 0 && aff[-1] == '1';
        s = ins ? 201 : 200;
        b.puts("{\"liked\":true,\"already_liked\":");
        b.puts(ins ? "false" : "true");
        b.puts(",\"post_id\":");
        char nb[24];
        char* ne = nb + sizeof(nb);
        char* ns = u64toa((uint64_t)r->id, ne);
        b.put(ns, (size_t)(ne - ns));
        b.ch('}');
      } else if (est == PGR_FATAL) {
        if (UNLIKELY(res->state[0] && !strcmp(res->state, "23503"))) {
          s = 404;
          b.puts("{\"error\":\"post not found\"}");
        } else {
          s = 500;
          b.puts("{\"error\":\"internal server error\"}");
        }
      } else {
        s = 500;
        b.puts("{\"error\":\"internal server error\"}");
      }
      break;
    }
  }
    return s;
  };
  if (LIKELY_P(r->gz, 0.85)) {
    Buf b(scratch, sizeof(scratch));
    st = build(b);
    if (LIKELY(!b.fail)) {
      body = scratch;
      blen = (size_t)(b.p - scratch);
    }
  } else {
    Buf b(r->rarea + RESP_PAD, RESP_CAP);
    st = build(b);
    if (LIKELY(!b.fail)) {
      body = r->rarea + RESP_PAD;
      blen = (size_t)(b.p - (r->rarea + RESP_PAD));
    } else {
      // oversized body: rebuild into scratch, takes the heap resp path
      Buf b2(scratch, sizeof(scratch));
      st = build(b2);
      if (LIKELY(!b2.fail)) {
        body = scratch;
        blen = (size_t)(b2.p - scratch);
      }
    }
  }
  if (UNLIKELY(!body)) {
    st = 500;
    body = "{\"error\":\"internal server error\"}";
    blen = strlen(body);
  }
  req_resp(r, st, body, blen);
  // writes stay queued until their pipeline sync: the sync's implicit
  // commit (and WAL flush) is the point where the write is durable
  if (!r->held) conn_drain(w, c);
}

// ------------------------------------------------------------------ dispatch

void dispatch(Worker* w, Conn* c, int method, const char* path, size_t plen,
              const char* authv, size_t authn, const char* body, size_t blen,
              bool gz) {
  Req* r = req_new(w, c);
  r->gz = gz;
  static const char POSTS[] = "/posts/";
  const size_t PP = 7;  // strlen("/posts/")

  if (LIKELY_P(method == M_GET, 0.8)) {
    // ordered by traffic: posts/:id dominates, then feed, health is rare
    if (plen > PP && !memcmp(path, POSTS, PP) &&
        !(plen > PP + 5 && !memcmp(path + plen - 5, "/like", 5))) {
      int64_t id;
      if (LIKELY(post_id(path + PP, plen - PP, &id))) {
        r->type = R_GET;
        r->id = id;
        pg_submit(w, r);
        return;
      }
      req_err(r, 400, "invalid post id");
    } else if (plen == 5 && !memcmp(path, "/feed", 5)) {
      r->type = R_FEED;
      pg_submit(w, r);
      return;
    } else if (plen == 7 && !memcmp(path, "/health", 7)) {
      r->type = R_HEALTH;
      pg_submit(w, r);
      return;
    } else {
      req_err(r, 404, "not found");
    }
    conn_drain(w, c);
    return;
  }

  if (method == M_POST) {
    bool is_create = plen == 6 && !memcmp(path, "/posts", 6);
    bool is_like = !is_create && plen > PP + 5 && !memcmp(path, POSTS, PP) &&
                   !memcmp(path + plen - 5, "/like", 5);
    if (UNLIKELY(!is_create && !is_like)) {
      req_err(r, 404, "not found");
      conn_drain(w, c);
      return;
    }
    // auth first, then id
    int64_t uid;
    int arc = auth_check(authv, authn, &uid, r->uname, sizeof(r->uname) - 1,
                         &r->ulen);
    if (UNLIKELY(arc)) {
      req_err(r, 401,
              arc == 1   ? "missing bearer token"
              : arc == 2 ? "invalid or expired token"
                         : "invalid token payload");
      conn_drain(w, c);
      return;
    }
    r->uid = uid;
    if (is_like) {
      int64_t id;
      if (LIKELY(post_id(path + PP, plen - PP - 5, &id))) {
        r->type = R_LIKE;
        r->id = id;
        pg_submit(w, r);
        return;
      }
      req_err(r, 400, "invalid post id");
    } else {
      uint16_t dl = 0;
      int jrc = json_body(body, blen, r->body, sizeof(r->body), &dl);
      if (UNLIKELY(jrc == 2)) {
        req_err(r, 400, "malformed JSON body");
      } else {
        // trim whitespace
        size_t a = 0, b = dl;
        while (a < b && isspace((uint8_t)r->body[a])) a++;
        while (b > a && isspace((uint8_t)r->body[b - 1])) b--;
        if (UNLIKELY(jrc == 1 || b == a)) {
          req_err(r, 400, "body is required");
        } else if (UNLIKELY(utf8_count(r->body + a, b - a) > 500)) {
          req_err(r, 400, "body must be at most 500 characters");
        } else {
          memmove(r->body, r->body + a, b - a);
          r->blen = (uint16_t)(b - a);
          r->body[r->blen] = 0;  // NUL-terminate for the text param length
          r->type = R_CREATE;
          pg_submit(w, r);
          return;
        }
      }
    }
    conn_drain(w, c);
    return;
  }

  // M_OTHER and anything unrouted
  req_err(r, 404, "not found");
  conn_drain(w, c);
}
