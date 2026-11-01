// PostgreSQL v3 wire-protocol client (replaces libpq on the request path).
//
// Connect + auth (trust/cleartext/SCRAM-SHA-256) runs blocking once per
// conn at setup; after that the socket is nonblocking and driven by the
// same epoll/io_uring machinery as before: pg_on_read parses frames,
// pg_pump appends Bind/Execute/Sync triples to a per-conn out buffer.
//
// Design notes:
//  - every client request is sent as Bind+Execute+Sync: one Sync per
//    request gives one implicit transaction per request, so errors stay
//    isolated (a failed query never rolls back a sibling) and exactly
//    one ReadyForQuery arrives per request.
//  - results are text format: field bytes point into the recv buffer and
//    are spliced into JSON verbatim (ids/counts need no conversion, and
//    put_ts already parses the text timestamptz).
//  - no Describe on the request path: column layouts are fixed and
//    validated once at prepare time.
//  - writes stay "held" until the ReadyForQuery after their Sync: the
//    commit happens at the sync point, not at CommandComplete.

#include "server.h"

#include <netdb.h>
#include <poll.h>
#include <sys/random.h>

// ---------------------------------------------------------------- conninfo

struct DbCfg {
  char host[256] = "localhost";
  char port[8] = "5432";
  char user[128] = "";
  char pass[256] = "";
  char db[128] = "";
  int connect_timeout = 5;
  int tcp_user_timeout = 10000;
  int ka_idle = 30, ka_int = 10, ka_cnt = 3;
};

static int pcd(const char* s, size_t n, char* d, size_t cap) {
  // rfc3986 percent-decoder for user/pass inside the URI authority
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == '%' && i + 2 < n) {
      auto h = [](char x) {
        if (x >= '0' && x <= '9') return x - '0';
        if (x >= 'a' && x <= 'f') return x - 'a' + 10;
        if (x >= 'A' && x <= 'F') return x - 'A' + 10;
        return -1;
      };
      int a = h(s[i + 1]), b = h(s[i + 2]);
      if (a < 0 || b < 0) return -1;
      c = (char)(a << 4 | b);
      i += 2;
    }
    if (o + 1 >= cap) return -1;
    d[o++] = c;
  }
  d[o] = 0;
  return 0;
}

static void cfg_opt(DbCfg* c, const char* k, size_t kn, const char* v,
                    size_t vn) {
  char key[64], val[512];
  if (kn >= sizeof(key)) return;
  memcpy(key, k, kn);
  key[kn] = 0;
  if (vn >= sizeof(val)) vn = sizeof(val) - 1;
  memcpy(val, v, vn);
  val[vn] = 0;
  if (!strcmp(key, "host")) strncpy(c->host, val, sizeof(c->host) - 1);
  else if (!strcmp(key, "port")) strncpy(c->port, val, sizeof(c->port) - 1);
  else if (!strcmp(key, "user")) strncpy(c->user, val, sizeof(c->user) - 1);
  else if (!strcmp(key, "password")) strncpy(c->pass, val, sizeof(c->pass) - 1);
  else if (!strcmp(key, "dbname")) strncpy(c->db, val, sizeof(c->db) - 1);
  else if (!strcmp(key, "connect_timeout")) c->connect_timeout = atoi(val);
  else if (!strcmp(key, "tcp_user_timeout")) c->tcp_user_timeout = atoi(val);
  else if (!strcmp(key, "keepalives_idle")) c->ka_idle = atoi(val);
  else if (!strcmp(key, "keepalives_interval")) c->ka_int = atoi(val);
  else if (!strcmp(key, "keepalives_count")) c->ka_cnt = atoi(val);
  // sslmode/gssencmode and unknown keys are ignored: we never send
  // SSLRequest/GSSENCRequest, which is exactly what disable means.
}

// accepts both "postgres://user:pass@host:port/db?k=v" and "k=v k=v"
static bool cfg_parse(DbCfg* c, const char* u) {
  const char* user_env = getenv("USER");
  if (user_env) strncpy(c->user, user_env, sizeof(c->user) - 1);
  if (!strncmp(u, "postgres://", 11) || !strncmp(u, "postgresql://", 14)) {
    u += u[7] == 'q' ? 14 : 11;  // "postgresql://" vs "postgres://"
    const char* e = u + strlen(u);
    const char* at = nullptr;
    for (const char* p = u; p < e; p++) {
      if (*p == '@') at = p;
      if (*p == '/' || *p == '?') break;
    }
    const char* host = u;
    if (at) {
      const char* colon = (const char*)memchr(u, ':', at - u);
      if (colon) {
        if (pcd(u, colon - u, c->user, sizeof(c->user)) ||
            pcd(colon + 1, at - colon - 1, c->pass, sizeof(c->pass)))
          return false;
      } else if (pcd(u, at - u, c->user, sizeof(c->user)))
        return false;
      host = at + 1;
    }
    const char* hend = host;
    while (hend < e && *hend != '/' && *hend != '?' && *hend != ':') hend++;
    if (hend > host) {
      size_t n = hend - host;
      if (n >= sizeof(c->host)) n = sizeof(c->host) - 1;
      memcpy(c->host, host, n);
      c->host[n] = 0;
    }
    const char* p = hend;
    if (p < e && *p == ':') {
      p++;
      const char* pe = p;
      while (pe < e && *pe != '/' && *pe != '?') pe++;
      size_t n = pe - p;
      if (n && n < sizeof(c->port)) {
        memcpy(c->port, p, n);
        c->port[n] = 0;
      }
      p = pe;
    }
    if (p < e && *p == '/') {
      p++;
      const char* de = p;
      while (de < e && *de != '?') de++;
      size_t n = de - p;
      if (n && n < sizeof(c->db)) {
        memcpy(c->db, p, n);
        c->db[n] = 0;
      }
      p = de;
    }
    if (p < e && *p == '?') {
      p++;
      while (p < e) {
        const char* ke = p;
        while (ke < e && *ke != '=' && *ke != '&') ke++;
        if (ke < e && *ke == '=') {
          const char* ve = ke + 1;
          while (ve < e && *ve != '&') ve++;
          cfg_opt(c, p, ke - p, ke + 1, ve - ke - 1);
          p = ve;
        } else
          p = ke;
        while (p < e && *p == '&') p++;
      }
    }
  } else {
    while (*u) {
      while (*u == ' ') u++;
      const char* ke = u;
      while (*ke && *ke != '=') ke++;
      if (!*ke) break;
      const char* vs = ke + 1;
      while (*vs == ' ') vs++;
      const char* ve;
      if (*vs == '\'') {
        vs++;
        ve = strchr(vs, '\'') ?: vs + strlen(vs);
        cfg_opt(c, u, ke - u, vs, ve - vs);
        u = *ve ? ve + 1 : ve;
      } else {
        ve = vs;
        while (*ve && *ve != ' ') ve++;
        cfg_opt(c, u, ke - u, vs, ve - vs);
        u = ve;
      }
    }
  }
  if (!*c->db) strncpy(c->db, c->user, sizeof(c->db) - 1);
  return c->user[0] != 0;
}

// ------------------------------------------------------------- conn state

// blocking-phase receive buffer: handshake messages are small and
// sequential so a fixed scratch is fine
struct Hs {
  int fd;
  char in[65536];
  uint32_t len = 0, pos = 0;
};

static bool hs_fill(Hs* h, int timeout_ms) {
  if (h->pos == h->len) h->pos = h->len = 0;
  if (h->len == sizeof(h->in)) return false;
  pollfd p = {h->fd, POLLIN, 0};
  int r = poll(&p, 1, timeout_ms);
  if (r <= 0 || !(p.revents & (POLLIN | POLLERR | POLLHUP))) return false;
  ssize_t n = recv(h->fd, h->in + h->len, sizeof(h->in) - h->len, 0);
  if (n <= 0) return false;
  h->len += n;
  return true;
}

// next complete frame: type + body span, or 0 when more input is needed
static int hs_frame(Hs* h, char* t, const char** body, uint32_t* blen,
                    int timeout_ms) {
  for (;;) {
    if (h->len - h->pos >= 5) {
      const uint8_t* m = (const uint8_t*)h->in + h->pos;
      uint32_t l = (uint32_t)m[1] << 24 | m[2] << 16 | m[3] << 8 | m[4];
      if (l < 4 || l > 1048576) return -1;
      if (h->len - h->pos < l + 1) {
        if (!hs_fill(h, timeout_ms)) return -1;
        continue;
      }
      *t = m[0];
      *body = (const char*)m + 5;
      *blen = l - 4;
      h->pos += l + 1;
      return 1;
    }
    if (!hs_fill(h, timeout_ms)) return -1;
  }
}

static bool hs_send(Hs* h, const void* b, size_t n) {
  const char* p = (const char*)b;
  while (n) {
    ssize_t k = send(h->fd, p, n, MSG_NOSIGNAL);
    if (k < 0) {
      if (errno == EINTR) continue;
      pollfd w = {h->fd, POLLOUT, 0};
      if (errno == EAGAIN && poll(&w, 1, 5000) > 0) continue;
      return false;
    }
    p += k;
    n -= k;
  }
  return true;
}

// ------------------------------------------------------- message builders

struct Mb {
  char* p;
  size_t n, cap;
  bool fail = false;
  void raw(const void* s, size_t k) {
    if (n + k > cap) {
      fail = true;
      return;
    }
    memcpy(p + n, s, k);
    n += k;
  }
  void u8(uint32_t v) {
    if (n + 1 > cap) {
      fail = true;
      return;
    }
    p[n++] = (char)v;
  }
  void u16(uint32_t v) {
    char b[2] = {(char)(v >> 8), (char)v};
    raw(b, 2);
  }
  void u32(uint32_t v) {
    char b[4] = {(char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v};
    raw(b, 4);
  }
  void cstr(const char* s) { raw(s, strlen(s) + 1); }
  void msg(char t, void (*f)(Mb*, void*), void* a) {
    u8(t);
    size_t lp = n;
    u32(0);
    f(this, a);
    uint32_t l = (uint32_t)(n - lp);
    p[lp] = l >> 24;
    p[lp + 1] = l >> 16;
    p[lp + 2] = l >> 8;
    p[lp + 3] = l;
  }
};

// ------------------------------------------------------------------ scram

struct Scram {
  char cnb[64];              // client nonce (b64)
  char cfb[128];             // client-first-message-bare
  int cflen;
  char sf[512];              // server-first-message verbatim
  int sflen;
  uint8_t skey[32];          // server key, kept for v= verification
  char amsg[1024];           // auth message assembled at client-final
  int amlen;
};

static int scram_first(Scram* s, char* out) {
  uint8_t rn[18];
  getrandom(rn, sizeof(rn), 0);
  int cn = b64_enc(rn, sizeof(rn), s->cnb);
  s->cnb[cn] = 0;
  s->cflen = snprintf(s->cfb, sizeof(s->cfb), "n=,r=%s", s->cnb);
  // full client-first = gs2 header + bare
  return snprintf(out, 160, "n,,%s", s->cfb);
}

// parse server-first, compute keys, emit client-final; -1 malformed
static int scram_final(Scram* s, const DbCfg* c, const char* msg, size_t n,
                       char* out, size_t ocap) {
  if (n >= sizeof(s->sf)) return -1;
  memcpy(s->sf, msg, n);
  s->sf[n] = 0;
  s->sflen = n;
  const char *r = nullptr, *sa = nullptr, *i = nullptr;
  size_t rn = 0, san = 0, in = 0;
  const char* p = s->sf;
  while (*p) {
    const char* ce = strchr(p, ',');
    size_t an = ce ? (size_t)(ce - p) : strlen(p);
    if (an > 2 && p[1] == '=') {
      if (p[0] == 'r') {
        r = p + 2;
        rn = an - 2;
      } else if (p[0] == 's') {
        sa = p + 2;
        san = an - 2;
      } else if (p[0] == 'i') {
        i = p + 2;
        in = an - 2;
      }
    }
    if (!ce) break;
    p = ce + 1;
  }
  if (!r || !sa || !i) return -1;
  size_t cnb_len = strlen(s->cnb);
  if (rn <= cnb_len || memcmp(r, s->cnb, cnb_len)) return -1;
  uint8_t salt[64];
  int slen = b64_dec(sa, san, salt);
  if (slen < 8 || slen > (int)sizeof(salt)) return -1;
  char ie[16];
  if (in >= sizeof(ie)) return -1;
  memcpy(ie, i, in);
  ie[in] = 0;
  int iter = atoi(ie);
  if (iter < 1 || iter > 1000000) return -1;

  // SaltedPassword = PBKDF2-HMAC-SHA256(pass, salt, iter, 32)
  uint8_t u[32], sp[32];
  {
    uint8_t sb[68];
    memcpy(sb, salt, slen);
    sb[slen] = 0;
    sb[slen + 1] = 0;
    sb[slen + 2] = 0;
    sb[slen + 3] = 1;
    hmac_sha256((const uint8_t*)c->pass, strlen(c->pass), sb, slen + 4, u);
    memcpy(sp, u, 32);
    for (int k = 1; k < iter; k++) {
      hmac_sha256((const uint8_t*)c->pass, strlen(c->pass), u, 32, u);
      for (int j = 0; j < 32; j++) sp[j] ^= u[j];
    }
  }
  uint8_t ckey[32], stored[32];
  hmac_sha256(sp, 32, (const uint8_t*)"Client Key", 10, ckey);
  sha256_oneshot(ckey, 32, stored);
  hmac_sha256(sp, 32, (const uint8_t*)"Server Key", 10, s->skey);

  // client-final-without-proof + assembled AuthMessage
  char cfp[512];
  int cfn = snprintf(cfp, sizeof(cfp), "c=biws,r=%.*s", (int)rn, r);
  s->amlen = snprintf(s->amsg, sizeof(s->amsg), "%.*s,%s,%s", s->cflen, s->cfb,
                      s->sf, cfp);
  uint8_t csig[32];
  hmac_sha256(stored, 32, (const uint8_t*)s->amsg, s->amlen, csig);
  uint8_t proof[32];
  for (int j = 0; j < 32; j++) proof[j] = ckey[j] ^ csig[j];
  char pb[48];
  int pn = b64_enc(proof, 32, pb);
  return snprintf(out, ocap, "%s,p=%.*s", cfp, pn, pb);
}

// verify server-final "v=<b64 ServerSignature>"; -1 mismatch/malformed
static int scram_verify(Scram* s, const char* msg, size_t n) {
  if (n < 3 || msg[0] != 'v' || msg[1] != '=') return -1;
  uint8_t sig[64];
  int sn = b64_dec(msg + 2, n - 2, sig);
  if (sn != 32) return -1;
  uint8_t want[32];
  hmac_sha256(s->skey, 32, (const uint8_t*)s->amsg, s->amlen, want);
  return memcmp(sig, want, 32) ? -1 : 0;
}

// ------------------------------------------------------------ handshake

// send a 'p' auth-phase message during the handshake
static bool hs_p(Hs* h, const void* b, size_t n) {
  char hdr[5];
  hdr[0] = 'p';
  uint32_t l = (uint32_t)n + 4;
  hdr[1] = l >> 24;
  hdr[2] = l >> 16;
  hdr[3] = l >> 8;
  hdr[4] = l;
  return hs_send(h, hdr, 5) && (n == 0 || hs_send(h, b, n));
}

// run the auth conversation until ReadyForQuery; returns false on any
// ErrorResponse/timeout/unsupported method
static bool hs_auth(Hs* h, const DbCfg* c) {
  Scram s;
  memset(&s, 0, sizeof(s));
  int phase = 0;  // 0 = password methods, 1 = scram continue, 2 = scram final
  for (;;) {
    char t;
    const char* b;
    uint32_t n;
    if (hs_frame(h, &t, &b, &n, c->connect_timeout * 1000) <= 0) return false;
    switch (t) {
      case 'R': {
        if (n < 4) return false;
        uint32_t code = (uint8_t)b[0] << 24 | (uint8_t)b[1] << 16 |
                        (uint8_t)b[2] << 8 | (uint8_t)b[3];
        if (code == 0) break;  // AuthenticationOk: keep waiting for Z
        if (code == 3) {
          if (!hs_p(h, c->pass, strlen(c->pass) + 1)) return false;
        } else if (code == 5) {
          return false;  // md5 auth: deprecated upstream, not implemented
        } else if (code == 10) {
          const char* mech = b + 4;
          bool found = false;
          while (mech < b + n && *mech) {
            if (!strcmp(mech, "SCRAM-SHA-256")) found = true;
            mech += strlen(mech) + 1;
          }
          if (!found) return false;
          char cf[160];
          int cfl = scram_first(&s, cf);
          // SASLInitialResponse: mechanism\0 + int32 len + client-first
          char m[256];
          Mb w = {m, 0, sizeof(m)};
          w.cstr("SCRAM-SHA-256");
          w.u32((uint32_t)cfl);
          w.raw(cf, cfl);
          if (w.fail || !hs_p(h, m, w.n)) return false;
          phase = 1;
        } else if (code == 11 && phase == 1) {
          char out[600];
          int on = scram_final(&s, c, b + 4, n - 4, out, sizeof(out));
          if (on < 0 || !hs_p(h, out, on)) return false;
          phase = 2;
        } else if (code == 12 && phase == 2) {
          if (scram_verify(&s, b + 4, n - 4)) return false;
          phase = 0;
        } else
          return false;
        break;
      }
      case 'E':
        return false;  // auth failures arrive as ErrorResponse (28P01)
      case 'S':
      case 'K':
      case 'N':
      case 'A':
      case 'v':
        break;  // parameter status / backend key / notices: consume
      case 'Z':
        return true;
      default:
        return false;
    }
  }
}

// run one simple 'Q' phase (setup commands / prepares) until its Z;
// returns false on ErrorResponse or timeout
static bool hs_query_ok(Hs* h, int timeout_ms) {
  for (;;) {
    char t;
    const char* b;
    uint32_t n;
    if (hs_frame(h, &t, &b, &n, timeout_ms) <= 0) return false;
    if (t == 'E') return false;
    if (t == 'Z') return true;
  }
}

// ------------------------------------------------------------- pgw_connect

struct Prep {
  const char* name;
  const char* sql;
};

static const Prep PREP[] = {
    {"feedq", "SELECT p.id, p.body, p.created_at, u.username, "
              "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int "
              "AS like_count FROM posts p JOIN users u ON u.id = p.user_id "
              "ORDER BY p.created_at DESC, p.id DESC LIMIT 20"},
    {"postq", "SELECT p.id, p.body, p.created_at, u.username, "
              "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int "
              "AS like_count FROM posts p JOIN users u ON u.id = p.user_id "
              "WHERE p.id = $1"},
    {"insq", "INSERT INTO posts (user_id, body) VALUES ($1, $2) "
             "RETURNING id, created_at"},
    {"likeq", "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) "
              "ON CONFLICT (user_id, post_id) DO NOTHING"},
    {"healthq", "SELECT 1"},
};

// TCP connect + startup + auth + prepare; blocking, once per conn.
// On success p->fd is a connected nonblocking socket and p->up may be set.
bool pgw_connect(Pg* p) {
  DbCfg c;
  if (!cfg_parse(&c, g_dburl)) return false;

  addrinfo hint{}, *ai = nullptr;
  hint.ai_family = AF_UNSPEC;
  hint.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(c.host, c.port, &hint, &ai) != 0 || !ai) return false;
  int fd = -1;
  for (addrinfo* a = ai; a && fd < 0; a = a->ai_next) {
    fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd < 0) continue;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    if (connect(fd, a->ai_addr, a->ai_addrlen) == 0 ||
        errno == EINPROGRESS) {
      pollfd w = {fd, POLLOUT, 0};
      if (poll(&w, 1, c.connect_timeout * 1000) > 0) {
        int err = 0;
        socklen_t el = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (!err) break;
      }
    }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(ai);
  if (fd < 0) return false;

  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &c.ka_idle, sizeof(c.ka_idle));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &c.ka_int, sizeof(c.ka_int));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &c.ka_cnt, sizeof(c.ka_cnt));
  if (c.tcp_user_timeout > 0)
    setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &c.tcp_user_timeout,
               sizeof(c.tcp_user_timeout));

  Hs h;
  h.fd = fd;
  h.len = h.pos = 0;

  // StartupMessage: no type byte, protocol 3.0, user/database + the two
  // session GUCs we want (both are USERSET so startup params work)
  {
    char sb[2048];
    Mb w = {sb, 0, sizeof(sb)};
    size_t lp = w.n;
    w.u32(0);
    w.u32(196608);
    w.cstr("user");
    w.cstr(c.user);
    w.cstr("database");
    w.cstr(c.db);
    w.cstr("client_encoding");
    w.cstr("UTF8");
    w.cstr("TimeZone");
    w.cstr("UTC");
    w.cstr("plan_cache_mode");
    w.cstr("force_generic_plan");
    w.u8(0);
    uint32_t l = (uint32_t)(w.n - lp);
    sb[lp] = l >> 24;
    sb[lp + 1] = l >> 16;
    sb[lp + 2] = l >> 8;
    sb[lp + 3] = l;
    if (w.fail || !hs_send(&h, sb, w.n)) goto bad;
  }

  if (!hs_auth(&h, &c)) goto bad;

  // prepare all five statements in one Parse batch + a single Sync;
  // an ErrorResponse here means the schema/queries don't match: drop
  {
    char pb[8192];
    Mb w = {pb, 0, sizeof(pb)};
    for (auto& q : PREP) {
      auto body = [](Mb* m, void* a) {
        auto* q = (Prep*)a;
        m->cstr(q->name);
        m->cstr(q->sql);
        m->u16(0);  // param types inferred
      };
      w.msg('P', body, (void*)&q);
    }
    w.u8('S');
    w.u32(4);
    if (w.fail || !hs_send(&h, pb, w.n)) goto bad;
    if (!hs_query_ok(&h, c.connect_timeout * 1000)) goto bad;
  }

  // keep whatever the handshake buffered: it is already inside the frame
  // protocol, so the runtime parser can continue from h.pos
  if (h.pos < h.len) {
    size_t rem = h.len - h.pos;
    if (rem > p->in_cap) {
      char* ni = (char*)malloc(h.len);
      if (!ni) goto bad;
      free(p->in);
      p->in = ni;
      p->in_cap = h.len;
    }
    memcpy(p->in, h.in + h.pos, rem);
    p->in_len = rem;
  }
  p->fd = fd;
  return true;
bad:
  close(fd);
  return false;
}

// -------------------------------------------------------- request encode

// append Bind+Execute for one request to p->out; -1 on encode failure.
// Sync is emitted separately by pg_pump so reads can share one per group.
static int enc_req(Pg* p, Req* r) {
  const char* stmt;
  char i1[24], i2[24];
  const char* pv[2];
  int pl[2];
  int np = 0;
  switch (r->type) {
    case R_FEED:
      stmt = "feedq";
      break;
    case R_GET:
      stmt = "postq";
      pv[0] = u64toa(r->id, i1 + sizeof(i1));
      pl[0] = (int)(i1 + sizeof(i1) - pv[0]);
      np = 1;
      break;
    case R_CREATE:
      stmt = "insq";
      pv[0] = u64toa(r->uid, i1 + sizeof(i1));
      pl[0] = (int)(i1 + sizeof(i1) - pv[0]);
      pv[1] = r->body;
      pl[1] = r->blen;
      np = 2;
      break;
    case R_LIKE:
      stmt = "likeq";
      pv[0] = u64toa(r->uid, i1 + sizeof(i1));
      pl[0] = (int)(i1 + sizeof(i1) - pv[0]);
      pv[1] = u64toa(r->id, i2 + sizeof(i2));
      pl[1] = (int)(i2 + sizeof(i2) - pv[1]);
      np = 2;
      break;
    default:
      stmt = "healthq";
      break;
  }
  // worst case need: ~96 bytes of headers + stmt name + params
  size_t need = 128 + strlen(stmt);
  for (int i = 0; i < np; i++) need += pl[i];
  if (p->out_len + need > p->out_cap) {
    uint32_t nc = p->out_cap ? p->out_cap : 16384;
    while (nc < p->out_len + need) nc *= 2;
    char* no = (char*)realloc(p->out, nc);
    if (!no) return -1;
    p->out = no;
    p->out_cap = nc;
  }
  Mb w = {p->out + p->out_len, 0, p->out_cap - p->out_len};
  struct Bind {
    const char* stmt;
    const char** pv;
    const int* pl;
    int np;
  } bd = {stmt, pv, pl, np};
  auto bbody = [](Mb* m, void* a) {
    auto* b = (Bind*)a;
    m->u8(0);             // unnamed portal
    m->cstr(b->stmt);
    m->u16(0);            // all params text
    m->u16(b->np);
    for (int i = 0; i < b->np; i++) {
      m->u32((uint32_t)b->pl[i]);
      m->raw(b->pv[i], b->pl[i]);
    }
    m->u16(0);            // all results text
  };
  w.msg('B', bbody, &bd);
  auto ebody = [](Mb* m, void*) {
    m->u8(0);             // unnamed portal
    m->u32(0);            // no row limit
  };
  w.msg('E', ebody, nullptr);
  if (w.fail) return -1;
  p->out_len += w.n;
  return 0;
}

// Sync closes a request group: reads batch many per Sync (they share one
// implicit transaction safely), writes always get their own (rollback must
// stay per request). Caller decides the boundary; pg.cpp sets r->grp.
int pgw_encode_sync(Pg* p) {
  if (p->out_len + 5 > p->out_cap) {
    uint32_t nc = p->out_cap ? p->out_cap * 2 : 16384;
    char* no = (char*)realloc(p->out, nc);
    if (!no) return -1;
    p->out = no;
    p->out_cap = nc;
  }
  p->out[p->out_len] = 'S';
  p->out[p->out_len + 1] = 0;
  p->out[p->out_len + 2] = 0;
  p->out[p->out_len + 3] = 0;
  p->out[p->out_len + 4] = 4;
  p->out_len += 5;
  return 0;
}

// -------------------------------------------------------------- io paths

// push pending output; returns 0 fully sent, 1 more to send, -1 conn dead
int pgw_flush(Pg* p) {
  while (p->out_off < p->out_len) {
    ssize_t n = send(p->fd, p->out + p->out_off, p->out_len - p->out_off,
                     MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
      return -1;
    }
    p->out_off += n;
  }
  p->out_len = p->out_off = 0;
  return 0;
}

int pgw_encode_req(Pg* p, Req* r) { return enc_req(p, r); }

void pgw_close(Pg* p) {
  if (p->fd >= 0) close(p->fd);
  p->fd = -1;
}

// ------------------------------------------------------------------ parse

static uint32_t be32(const char* p) {
  const uint8_t* m = (const uint8_t*)p;
  return (uint32_t)m[0] << 24 | m[1] << 16 | m[2] << 8 | m[3];
}

// dispatch one complete frame; 0 ok, -1 protocol violation (drop conn).
// emits finished results through req_pg_done per sent-queue order.
static int on_msg(Worker* w, Pg* p, char t, const char* b, uint32_t n) {
  switch (t) {
    case '1':  // ParseComplete
    case '2':  // BindComplete
    case '3':  // CloseComplete
    case 'n':  // NoData
    case 's':  // PortalSuspended (never: row limit is always 0)
    case 't':  // ParameterDescription
    case 'T':  // RowDescription (never without Describe)
    case 'K':  // BackendKeyData
    case 'A':  // NotificationResponse
    case 'v':  // NegotiateProtocolVersion (never for protocol 3.0)
    case 'd':  // CopyData
    case 'c':  // CopyDone
    case 'G':  // CopyInResponse
    case 'H':  // CopyOutResponse
    case 'W':  // CopyBothResponse
    case 'N':  // NoticeResponse: consume, ignore
      return 0;

    case 'D': {  // DataRow
      if (n < 2) return -1;
      uint16_t nc = (uint8_t)b[0] << 8 | (uint8_t)b[1];
      if (p->fcols == 0) p->fcols = nc;
      if (nc != p->fcols) return -1;  // column model broken: desync
      const char* q = b + 2;
      const char* e = b + n;
      for (int i = 0; i < nc; i++) {
        if (e - q < 4) return -1;
        int32_t l = (int32_t)be32(q);
        q += 4;
        if (l == -1) {
          p->flds.push_back({q, -1});
        } else {
          if (l < 0 || e - q < l) return -1;
          p->flds.push_back({q, l});
          q += l;
        }
      }
      if (q != e) return -1;
      p->res.nrows++;
      return 0;
    }

    case 'C':  // CommandComplete
    case 'I':  // EmptyQueryResponse
    case 'E': {  // ErrorResponse
      Req* r = p->sent.empty() ? nullptr : p->sent.front();
      if (!r) {
        // a result with no matching request: connection desync
        return -1;
      }
      p->sent.pop_front();
      p->res.tag = nullptr;
      p->res.taglen = 0;
      p->res.state[0] = 0;
      p->res.status = t == 'E' ? PGR_FATAL : PGR_COMMAND;
      if (t == 'C') {
        p->res.tag = b;
        p->res.taglen = (int)n - 1;  // tag is NUL terminated
        if (p->res.taglen < 0) p->res.taglen = 0;
        // SELECTs report TUPLES even at zero rows; INSERT..RETURNING
        // reports rows but carries an "INSERT" tag, so rows win too
        if (p->res.nrows ||
            (p->res.taglen >= 6 && !memcmp(b, "SELECT", 6)))
          p->res.status = PGR_TUPLES;
      }
      p->res.ncols = p->fcols;
      p->res.rows = p->flds.data();
      if (t == 'E') {
        // field-set: byte1 code + string, NUL terminated list
        const char* q = b;
        const char* e = b + n;
        int fatal = 0;
        while (q < e && *q) {
          char code = *q++;
          const char* v = q;
          const char* ve = (const char*)memchr(q, 0, e - q);
          if (!ve) return -1;
          if (code == 'C' && ve - v >= 5) {
            memcpy(p->res.state, v, 5);
            p->res.state[5] = 0;
          } else if ((code == 'S' || code == 'V') && ve - v >= 5 &&
                     !memcmp(v, "FATAL", 5))
            fatal = 1;
          q = ve + 1;
        }
        p->res.status = PGR_FATAL;
        p->err_grp = r->grp;  // PG skips to Sync: the rest of the
                              // errored request's group is aborted
        if (fatal) {
          // backend is dying: request fails, conn follows
          req_pg_done(w, r, &p->res);
          p->flds.clear();
          p->res.nrows = 0;
          p->fcols = 0;
          return -1;
        }
      }
      // writes hold their response until the Sync's ReadyForQuery:
      // the implicit commit happens there, not at CommandComplete
      if ((r->type == R_CREATE || r->type == R_LIKE) && r->c) {
        r->held = 1;
        p->held = r;
      }
      req_pg_done(w, r, &p->res);
      p->flds.clear();
      p->res.nrows = 0;
      p->fcols = 0;
      return 0;
    }

    case 'Z': {  // ReadyForQuery: one per Sync; the commit signal
      if (n < 1 || b[0] != 'I') return -1;  // 'T'/'E': poisoned session
      if (p->err_grp) {
        // the errored request's group aborted everything after it: those
        // requests never got results, fail them now that the group ended
        while (!p->sent.empty() && p->sent.front()->grp == p->err_grp) {
          Req* a = p->sent.front();
          p->sent.pop_front();
          a->on_pg = 0;
          if (a->c) {
            req_err(a, 500, "internal server error");
            conn_drain(w, a->c);
          } else {
            req_free(w, a);
          }
        }
        p->err_grp = 0;
      }
      if (p->held) {
        Req* r = p->held;
        p->held = nullptr;
        r->held = 0;
        if (r->c) conn_drain(w, r->c);
        else req_free(w, r);
      }
      return 0;
    }
  }
  return 0;  // unknown types: skip by validated length
}

// recv till EAGAIN and dispatch every complete frame
void pgw_recv_parse(Worker* w, Pg* p) {
  uint32_t pos = 0;
  for (;;) {
    if (p->in_len == p->in_cap) {
      uint32_t nc = p->in_cap ? p->in_cap * 2 : 65536;
      if (nc > 16 * 1024 * 1024) {  // results here are <= ~16KB
        goto dead;
      }
      char* ni = (char*)realloc(p->in, nc);
      if (!ni) goto dead;
      p->in = ni;
      p->in_cap = nc;
    }
    ssize_t n = recv(p->fd, p->in + p->in_len, p->in_cap - p->in_len, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (getenv("PGDBG")) fprintf(stderr, "pgw_recv err=%d\n", errno);
      goto dead;
    }
    if (n == 0) {
      if (getenv("PGDBG")) fprintf(stderr, "pgw_recv eof\n");
      goto dead;
    }
    p->in_len += n;

    while (p->in_len - pos >= 5) {
      const char* m = p->in + pos;
      uint32_t l = be32(m + 1);
      if (l < 4 || l > 16 * 1024 * 1024) {
        if (getenv("PGDBG"))
          fprintf(stderr, "pgw badlen %u type=%c\n", l, m[0]);
        goto dead;
      }
      if (p->in_len - pos < l + 1) break;
      if (on_msg(w, p, m[0], m + 5, l - 4)) {
        if (getenv("PGDBG"))
          fprintf(stderr, "pgw msg err type=%c len=%u\n", m[0], l);
        goto dead;
      }
      pos += l + 1;
    }
  }
  if (pos) {
    memmove(p->in, p->in + pos, p->in_len - pos);
    p->in_len -= pos;
  }
  return;
dead:
  pg_down(w, p);
}
