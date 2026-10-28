// Request lifecycle and routing: request objects are recycled per worker,
// routed in dispatch(), and completed either inline (errors) or by
// req_pg_done() when the database answers. Responses are built into the
// request's own inline area (header written in front of the serialized
// body), so the hot path allocates nothing.

#include "server.h"

#include <libdeflate.h>

// ------------------------------------------------------------------ req lifecycle

Req* req_new(Worker* w, Conn* c) {
  Req* r;
  if (!w->rfree.empty()) {
    r = w->rfree.back();
    w->rfree.pop_back();
  } else {
    r = (Req*)calloc(1, sizeof(Req));
    if (!r) die("oom");
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
  return r;
}

void req_free(Worker* w, Req* r) {
  if (r->resp_kind == RK_HEAP) free(r->resp);
  r->resp = nullptr;
  r->resp_kind = RK_NONE;
  r->held = 0;
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
  const char* rsn = reason_of(status);
  size_t rn = strlen(rsn);
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
  if (r->gz && blen >= 1024) {
    if (!w->gzc) w->gzc = libdeflate_alloc_compressor(1);
    if (w->gzc &&
        libdeflate_gzip_compress_bound((libdeflate_compressor*)w->gzc, blen) <= RESP_CAP) {
      size_t gn = libdeflate_gzip_compress((libdeflate_compressor*)w->gzc,
                                           body, blen, r->rarea + RESP_PAD, RESP_CAP);
      if (gn) {
        out = r->rarea + RESP_PAD;
        olen = gn;
        gzout = true;
      }
    }
  }
  if (!gzout && blen <= RESP_CAP) {
    if (body != r->rarea + RESP_PAD) memcpy(r->rarea + RESP_PAD, body, blen);
    out = r->rarea + RESP_PAD;
  }
  char hb[RESP_PAD];
  char* he = hdr_build(hb, status, olen, gzout);
  size_t hn = (size_t)(he - hb);
  if (out == r->rarea + RESP_PAD) {
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

// complete responses for every spec-mandated error body, built once
void req_err(Req* r, int status, const char* msg) {
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
  // date part YYYY-MM-DD
  if (slen < 19) {
    b.put(s, (size_t)slen);
    return;
  }
  b.put(s, 10);
  b.ch('T');
  b.put(s + 11, 8);
  b.ch('.');
  if (s[19] == '.') {
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
void req_pg_done(Worker* w, Req* r, PGresult* res) {
  Conn* c = r->c;
  r->on_pg = 0;
  if (!c) {
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
    ExecStatusType est = PQresultStatus(res);
  switch (r->type) {
    case R_HEALTH:
      if (est == PGRES_TUPLES_OK) {
        b.puts("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
        char nb[24];
        char* ne = nb + sizeof(nb);
        char* ns = u64toa((uint64_t)(time(nullptr) - g_start), ne);
        b.put(ns, (size_t)(ne - ns));
        b.ch('}');
      } else {
        s = 503;
        b.puts("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\""
               "db query failed\"}");
      }
      break;
    case R_FEED: {
      if (est != PGRES_TUPLES_OK) {
        s = 500;
        b.puts("{\"error\":\"internal server error\"}");
        break;
      }
      int rows = PQntuples(res);
      b.puts("{\"posts\":[");
      for (int i = 0; i < rows; i++) {
        if (i) b.ch(',');
        b.puts("{\"id\":");
        b.put(PQgetvalue(res, i, 0), (size_t)PQgetlength(res, i, 0));
        b.puts(",\"body\":\"");
        b.jesc(PQgetvalue(res, i, 1), PQgetlength(res, i, 1));
        b.puts("\",\"created_at\":\"");
        put_ts(b, PQgetvalue(res, i, 2), PQgetlength(res, i, 2));
        b.puts("\",\"author\":\"");
        b.jesc(PQgetvalue(res, i, 3), PQgetlength(res, i, 3));
        b.puts("\",\"like_count\":");
        b.put(PQgetvalue(res, i, 4), (size_t)PQgetlength(res, i, 4));
        b.ch('}');
      }
      b.puts("]}");
      break;
    }
    case R_GET: {
      if (est != PGRES_TUPLES_OK) {
        s = 500;
        b.puts("{\"error\":\"internal server error\"}");
        break;
      }
      if (PQntuples(res) == 0) {
        s = 404;
        b.puts("{\"error\":\"post not found\"}");
        break;
      }
      b.puts("{\"post\":{\"id\":");
      b.put(PQgetvalue(res, 0, 0), (size_t)PQgetlength(res, 0, 0));
      b.puts(",\"body\":\"");
      b.jesc(PQgetvalue(res, 0, 1), PQgetlength(res, 0, 1));
      b.puts("\",\"created_at\":\"");
      put_ts(b, PQgetvalue(res, 0, 2), PQgetlength(res, 0, 2));
      b.puts("\",\"author\":\"");
      b.jesc(PQgetvalue(res, 0, 3), PQgetlength(res, 0, 3));
      b.puts("\",\"like_count\":");
      b.put(PQgetvalue(res, 0, 4), (size_t)PQgetlength(res, 0, 4));
      b.puts("}}");
      break;
    }
    case R_CREATE: {
      if (est == PGRES_TUPLES_OK && PQntuples(res) == 1) {
        s = 201;
        b.puts("{\"post\":{\"id\":");
        b.put(PQgetvalue(res, 0, 0), (size_t)PQgetlength(res, 0, 0));
        b.puts(",\"body\":\"");
        b.jesc(r->body, r->blen);
        b.puts("\",\"created_at\":\"");
        put_ts(b, PQgetvalue(res, 0, 1), PQgetlength(res, 0, 1));
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
      if (est == PGRES_COMMAND_OK) {
        const char* aff = PQcmdTuples(res);
        bool ins = aff[0] == '1';
        s = ins ? 201 : 200;
        b.puts("{\"liked\":true,\"already_liked\":");
        b.puts(ins ? "false" : "true");
        b.puts(",\"post_id\":");
        char nb[24];
        char* ne = nb + sizeof(nb);
        char* ns = u64toa((uint64_t)r->id, ne);
        b.put(ns, (size_t)(ne - ns));
        b.ch('}');
      } else if (est == PGRES_FATAL_ERROR) {
        const char* sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
        if (sqlstate && !strcmp(sqlstate, "23503")) {
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
  if (r->gz) {
    Buf b(scratch, sizeof(scratch));
    st = build(b);
    if (!b.fail) {
      body = scratch;
      blen = (size_t)(b.p - scratch);
    }
  } else {
    Buf b(r->rarea + RESP_PAD, RESP_CAP);
    st = build(b);
    if (!b.fail) {
      body = r->rarea + RESP_PAD;
      blen = (size_t)(b.p - (r->rarea + RESP_PAD));
    } else {
      // oversized body: rebuild into scratch, takes the heap resp path
      Buf b2(scratch, sizeof(scratch));
      st = build(b2);
      if (!b2.fail) {
        body = scratch;
        blen = (size_t)(b2.p - scratch);
      }
    }
  }
  if (!body) {
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

  if (method == M_GET) {
    if (plen == 7 && !memcmp(path, "/health", 7)) {
      r->type = R_HEALTH;
      pg_submit(w, r);
      return;
    }
    if (plen == 5 && !memcmp(path, "/feed", 5)) {
      r->type = R_FEED;
      pg_submit(w, r);
      return;
    }
    if (plen > PP && !memcmp(path, POSTS, PP) &&
        !(plen > PP + 5 && !memcmp(path + plen - 5, "/like", 5))) {
      int64_t id;
      if (!post_id(path + PP, plen - PP, &id)) {
        req_err(r, 400, "invalid post id");
      } else {
        r->type = R_GET;
        r->id = id;
        pg_submit(w, r);
        return;
      }
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
    if (!is_create && !is_like) {
      req_err(r, 404, "not found");
      conn_drain(w, c);
      return;
    }
    // auth first, then id
    int64_t uid;
    int arc = auth_check(authv, authn, &uid, r->uname, sizeof(r->uname) - 1,
                         &r->ulen);
    if (arc) {
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
      if (!post_id(path + PP, plen - PP - 5, &id)) {
        req_err(r, 400, "invalid post id");
      } else {
        r->type = R_LIKE;
        r->id = id;
        pg_submit(w, r);
        return;
      }
    } else {
      uint16_t dl = 0;
      int jrc = json_body(body, blen, r->body, sizeof(r->body), &dl);
      if (jrc == 2) {
        req_err(r, 400, "malformed JSON body");
      } else {
        // trim whitespace
        size_t a = 0, b = dl;
        while (a < b && isspace((uint8_t)r->body[a])) a++;
        while (b > a && isspace((uint8_t)r->body[b - 1])) b--;
        if (jrc == 1 || b == a) {
          req_err(r, 400, "body is required");
        } else if (utf8_count(r->body + a, b - a) > 500) {
          req_err(r, 400, "body must be at most 500 characters");
        } else {
          memmove(r->body, r->body + a, b - a);
          r->blen = (uint16_t)(b - a);
          r->body[r->blen] = 0;  // libpq wants NUL-terminated text params
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
