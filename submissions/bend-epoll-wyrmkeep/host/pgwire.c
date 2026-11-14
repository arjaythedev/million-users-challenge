// The PostgreSQL wire client (host/pgwire.h; docs/specs/S25-the-framer.md; "The wire client" in
// bench/servers/bend-epoll/README.md). Compiled as its own unit beside the generated
// bend-epoll.c (bench/servers/bend-epoll/build.sh); it knows nothing of the Bend runtime.
// Outside every proof.
//
// The connect: one socket per connection, non-blocking, TCP_NODELAY and SO_KEEPALIVE set as
// libpq set them, to the one address DATABASE_URL's host resolved to when the process
// started (pgw_target). The startup: a StartupMessage for protocol 3.0 with user, database and
// the caller's parameters (host/pg.c: application_name, options), client_encoding UTF8 (the
// render writes the rows' bytes into JSON as UTF-8); then AuthenticationOk, a cleartext
// password, or SCRAM-SHA-256 (RFC 5802, RFC 7677) without channel binding: gs2 header "n,,",
// the password as DATABASE_URL has it after percent-decoding, not SASLprep'd, the server's
// nonce checked to extend the client's, and the server's signature checked before
// AuthenticationOk is accepted. The SCRAM salting (pgw_salted) runs on the caller's event loop,
// once per salt and iteration count (the last result is kept): the count is refused above
// PGW_ITER_MAX, 1,000,000 (PostgreSQL's default is 4096), so a server's count stalls the loop
// for at most that many HMACs. ParameterStatus is read for integer_datetimes (binary
// timestamps are 8-byte integers only when it is on) and otherwise ignored; BackendKeyData's
// pid and key are kept for the cancel; ReadyForQuery ends the startup. An ErrorResponse, an
// authentication method other than those three, or a message out of place fails it.
//
// The CancelRequest: a fresh non-blocking connect to the same address, the 16 bytes
// (length 16, code 80877102, pid, key), then a wait for the server to close, as libpq's
// PQcancelPoll waits; host/pg.c bounds the whole by --db-timeout.

#define _GNU_SOURCE
#define OPENSSL_SUPPRESS_DEPRECATED  // SHA256_Init/Update/Final, as host/http.h uses them
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include "pgwire.h"

enum { PH_CONNECT, PH_STARTUP, PH_UP };

#define PGW_FIRST (64u << 10)  // a receive buffer's first size
#define PGW_ROOM  (16u << 10)  // free bytes a recv gets at least
#define PGW_SALT  128u         // a longer SCRAM salt fails the connection
#define PGW_ITER_MAX 1000000u  // a larger SCRAM iteration count fails the connection

// The one target of the process: DATABASE_URL, parsed and resolved once (pgw_target).
static struct {
  struct sockaddr_storage addr;
  socklen_t               alen;
  char*                   user;
  char*                   pass;  // NULL: the URL has none
  char*                   db;
  const char* const*      params;
} tg;

static int pgw_failc(PgwConn* c, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static int pgw_failc(PgwConn* c, const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  vsnprintf(c->err, sizeof c->err, fmt, a);
  va_end(a);
  return PGW_FAIL;
}

static void* pgw_alloc(void* p, size_t n) {
  void* q = realloc(p, n);
  if (q == NULL) {
    abort();
  }
  return q;
}

// ---- DATABASE_URL ----

static int pgw_hex(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// [p, p + n) percent-decoded into a new string; NULL on a bad escape or an escaped NUL.
static char* pgw_dec(const char* p, size_t n) {
  char*  s = pgw_alloc(NULL, n + 1);
  size_t k = 0;
  for (size_t i = 0; i < n; i += 1) {
    if (p[i] != '%') {
      s[k++] = p[i];
      continue;
    }
    int hi = i + 2 < n ? pgw_hex(p[i + 1]) : -1;  // both digits inside [p, p + n)
    int lo = i + 2 < n ? pgw_hex(p[i + 2]) : -1;
    if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) {
      free(s);
      return NULL;
    }
    s[k++] = (char)(hi * 16 + lo);
    i += 2;
  }
  s[k] = 0;
  return s;
}

static bool pgw_is(const char* s, const char* const* ok) {
  for (; *ok != NULL; ok += 1) {
    if (!strcmp(s, *ok)) {
      return true;
    }
  }
  return false;
}

#define URL_ERR(...)                      \
  do {                                    \
    snprintf(err, errn, __VA_ARGS__);     \
    goto fail;                            \
  } while (0)

int pgw_target(const char* url, const char* const* params, char* err, size_t errn) {
  static const char* const ssl_ok[] = { "disable", "allow", "prefer", NULL };
  static const char* const gss_ok[] = { "disable", "prefer", NULL };
  static const char* const ignored[] = { "application_name", "options", "connect_timeout", NULL };
  char *user = NULL, *pass = NULL, *host = NULL, *port = NULL, *db = NULL, *key = NULL, *val = NULL;
  const char* p;
  if (!strncmp(url, "postgresql://", 13)) {
    p = url + 13;
  } else if (!strncmp(url, "postgres://", 11)) {
    p = url + 11;
  } else {
    URL_ERR("DATABASE_URL is not a postgres:// or postgresql:// URL");
  }
  const char* q = p + strcspn(p, "@/");  // user information ends at the first '@' before any '/', as libpq reads it
  if (*q == '@') {
    const char* colon = memchr(p, ':', (size_t)(q - p));
    user = pgw_dec(p, (size_t)((colon != NULL ? colon : q) - p));
    if (colon != NULL) {
      pass = pgw_dec(colon + 1, (size_t)(q - colon - 1));
      if (pass == NULL) {
        URL_ERR("DATABASE_URL: the password has a bad percent escape");
      }
    }
    if (user == NULL) {
      URL_ERR("DATABASE_URL: the user has a bad percent escape");
    }
    p = q + 1;
  }
  if (*p == '[') {
    const char* e = strchr(p, ']');
    if (e == NULL) {
      URL_ERR("DATABASE_URL: an IPv6 host without its ']'");
    }
    host = pgw_dec(p + 1, (size_t)(e - p - 1));
    p    = e + 1;
  } else {
    size_t n = strcspn(p, ":/?,");
    host     = pgw_dec(p, n);
    p += n;
  }
  if (host == NULL) {
    URL_ERR("DATABASE_URL: the host has a bad percent escape");
  }
  if (*p == ',') {
    URL_ERR("DATABASE_URL names more than one host; this client connects to one");
  }
  if (*p == ':') {
    p += 1;
    size_t n = strcspn(p, "/?");
    port     = pgw_dec(p, n);
    p += n;
  }
  if (*p == '/') {
    p += 1;
    size_t n = strcspn(p, "?");
    db       = pgw_dec(p, n);
    p += n;
    if (db == NULL) {
      URL_ERR("DATABASE_URL: the database name has a bad percent escape");
    }
  }
  if (*p == '?') {
    p += 1;
  }
  while (*p != 0) {
    size_t      n  = strcspn(p, "&");
    const char* eq = memchr(p, '=', n);
    if (eq == NULL) {
      URL_ERR("DATABASE_URL: a query parameter without '='");
    }
    key = pgw_dec(p, (size_t)(eq - p));
    val = pgw_dec(eq + 1, (size_t)(p + n - eq - 1));
    if (key == NULL || val == NULL) {
      URL_ERR("DATABASE_URL: a query parameter with a bad percent escape");
    }
    if (!strcmp(key, "sslmode")) {
      if (!pgw_is(val, ssl_ok)) {
        URL_ERR("DATABASE_URL: sslmode=%s needs SSL, which this client does not speak", val);
      }
    } else if (!strcmp(key, "gssencmode")) {
      if (!pgw_is(val, gss_ok)) {
        URL_ERR("DATABASE_URL: gssencmode=%s needs GSS encryption, which this client does not speak", val);
      }
    } else if (!strcmp(key, "host") || !strcmp(key, "port") || !strcmp(key, "user") || !strcmp(key, "password")
      || !strcmp(key, "dbname")) {
      char** slot = key[0] == 'h' ? &host : key[0] == 'u' ? &user : key[1] == 'o' ? &port : key[0] == 'p' ? &pass : &db;
      free(*slot);
      *slot = val;
      val   = NULL;
    } else if (!pgw_is(key, ignored)) {
      URL_ERR("DATABASE_URL: the query parameter %s is not supported by this client", key);
    }
    free(key);
    free(val);
    key = val = NULL;
    p += n;
    if (*p == '&') {
      p += 1;
    }
  }
  if (host[0] == 0 || host[0] == '/') {
    URL_ERR("DATABASE_URL names no TCP host (this client does not use Unix sockets)");
  }
  if (user == NULL || user[0] == 0) {
    URL_ERR("DATABASE_URL names no user");
  }
  if (port != NULL && (port[0] == 0 || strspn(port, "0123456789") != strlen(port) || strlen(port) > 5 || atoi(port) < 1
                        || atoi(port) > 65535)) {
    URL_ERR("DATABASE_URL: the port is not a number from 1 to 65535");
  }
  struct addrinfo  hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_NUMERICSERV };
  struct addrinfo* ai    = NULL;
  int              gai   = getaddrinfo(host, port != NULL ? port : "5432", &hints, &ai);
  if (gai != 0 || ai == NULL) {
    URL_ERR("DATABASE_URL: the host %s does not resolve: %s", host, gai_strerror(gai));
  }
  memcpy(&tg.addr, ai->ai_addr, ai->ai_addrlen);
  tg.alen = ai->ai_addrlen;
  freeaddrinfo(ai);
  tg.user   = user;
  tg.pass   = pass;
  tg.db     = db != NULL && db[0] != 0 ? db : strdup(user);
  tg.params = params;
  if (tg.db == NULL) {
    abort();
  }
  if (db != NULL && db[0] == 0) {
    free(db);
  }
  free(host);
  free(port);
  return 0;
fail:
  free(user);
  free(pass);
  free(host);
  free(port);
  free(db);
  free(key);
  free(val);
  return -1;
}

// ---- base64 (RFC 4648, the standard alphabet, padded) ----

static const char pgw_b64c[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t pgw_b64(const unsigned char* in, size_t n, char* out) {
  size_t k = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
    out[k++]   = pgw_b64c[v >> 18 & 63];
    out[k++]   = pgw_b64c[v >> 12 & 63];
    out[k++]   = i + 1 < n ? pgw_b64c[v >> 6 & 63] : '=';
    out[k++]   = i + 2 < n ? pgw_b64c[v & 63] : '=';
  }
  out[k] = 0;
  return k;
}

static int pgw_b64v(char c) {
  const char* p = c != 0 ? strchr(pgw_b64c, c) : NULL;
  return p != NULL ? (int)(p - pgw_b64c) : -1;
}

// Strict decode: a multiple of 4 characters, '=' only as the padding. The byte count, or -1
// (also when it would pass cap).
static long pgw_unb64(const char* s, size_t n, unsigned char* out, size_t cap) {
  if (n % 4 != 0) {
    return -1;
  }
  size_t k = 0;
  for (size_t i = 0; i < n; i += 4) {
    int  v[4];
    bool last = i + 4 == n;
    for (int j = 0; j < 4; j += 1) {
      v[j] = pgw_b64v(s[i + j]);
    }
    int pad = last && s[i + 3] == '=' ? (s[i + 2] == '=' ? 2 : 1) : 0;
    if (v[0] < 0 || v[1] < 0 || (pad < 2 && v[2] < 0) || (pad < 1 && v[3] < 0)) {
      return -1;
    }
    if (k + 3 - (size_t)pad > cap) {
      return -1;
    }
    uint32_t w = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)(pad < 2 ? v[2] : 0) << 6 | (uint32_t)(pad < 1 ? v[3] : 0);
    out[k++] = (unsigned char)(w >> 16);
    if (pad < 2) {
      out[k++] = (unsigned char)(w >> 8);
    }
    if (pad < 1) {
      out[k++] = (unsigned char)w;
    }
  }
  return (long)k;
}

// ---- HMAC-SHA-256 and Hi() (RFC 2104, RFC 5802 §2.2) ----

typedef struct {
  SHA256_CTX in, out;  // the key's ipad and opad blocks absorbed
} PgwMac;

static void pgw_mac_key(PgwMac* m, const void* key, size_t n) {
  unsigned char k[SHA256_CBLOCK] = { 0 }, ip[SHA256_CBLOCK], op[SHA256_CBLOCK];
  if (n > SHA256_CBLOCK) {
    SHA256(key, n, k);
  } else {
    memcpy(k, key, n);
  }
  for (int i = 0; i < SHA256_CBLOCK; i += 1) {
    ip[i] = k[i] ^ 0x36;
    op[i] = k[i] ^ 0x5c;
  }
  SHA256_Init(&m->in);
  SHA256_Update(&m->in, ip, sizeof ip);
  SHA256_Init(&m->out);
  SHA256_Update(&m->out, op, sizeof op);
  OPENSSL_cleanse(k, sizeof k);
  OPENSSL_cleanse(ip, sizeof ip);
  OPENSSL_cleanse(op, sizeof op);
}

static void pgw_mac(const PgwMac* m, const void* msg, size_t n, unsigned char mac[32]) {
  unsigned char t[32];
  SHA256_CTX    c = m->in;
  SHA256_Update(&c, msg, n);
  SHA256_Final(t, &c);
  c = m->out;
  SHA256_Update(&c, t, sizeof t);
  SHA256_Final(mac, &c);
}

// SaltedPassword = Hi(password, salt, i). Every connection of the process logs in as the same
// role, so the server sends the same salt and count each time: the last result is kept.
static struct {
  bool          ok;
  uint32_t      iter, sn;
  unsigned char salt[PGW_SALT], sp[32];
} pgw_hi;

static void pgw_salted(const unsigned char* salt, uint32_t sn, uint32_t iter, unsigned char out[32]) {
  if (pgw_hi.ok && pgw_hi.iter == iter && pgw_hi.sn == sn && !memcmp(pgw_hi.salt, salt, sn)) {
    memcpy(out, pgw_hi.sp, 32);
    return;
  }
  PgwMac        k;
  unsigned char first[PGW_SALT + 4], u[32];
  pgw_mac_key(&k, tg.pass, strlen(tg.pass));
  memcpy(first, salt, sn);
  memcpy(first + sn, "\0\0\0\1", 4);
  pgw_mac(&k, first, sn + 4, u);
  memcpy(out, u, 32);
  for (uint32_t i = 1; i < iter; i += 1) {
    pgw_mac(&k, u, 32, u);
    for (int j = 0; j < 32; j += 1) {
      out[j] ^= u[j];
    }
  }
  pgw_hi.ok   = true;
  pgw_hi.iter = iter;
  pgw_hi.sn   = sn;
  memcpy(pgw_hi.salt, salt, sn);
  memcpy(pgw_hi.sp, out, 32);
  OPENSSL_cleanse(&k, sizeof k);
  OPENSSL_cleanse(u, sizeof u);
}

// ---- the send buffer and the encoders ----

// n more bytes at the end of the send buffer.
static char* pgw_put(PgwConn* c, uint32_t n) {
  PgwOut* o = &c->out;
  if (o->off == o->n) {
    o->off = o->n = 0;
  }
  if (o->cap - o->n < n) {
    if (o->off > 0) {
      memmove(o->p, o->p + o->off, o->n - o->off);
      o->n -= o->off;
      o->off = 0;
    }
    if (o->cap - o->n < n) {
      uint32_t cap = o->cap ? o->cap : 16384;
      while (cap - o->n < n) {
        cap *= 2;
      }
      o->p   = pgw_alloc(o->p, cap);
      o->cap = cap;
    }
  }
  char* p = o->p + o->n;
  o->n += n;
  return p;
}

static char* pgw_w32(char* p, uint32_t v) {
  p[0] = (char)(v >> 24);
  p[1] = (char)(v >> 16);
  p[2] = (char)(v >> 8);
  p[3] = (char)v;
  return p + 4;
}

static char* pgw_w16(char* p, uint16_t v) {
  p[0] = (char)(v >> 8);
  p[1] = (char)v;
  return p + 2;
}

static char* pgw_wmem(char* p, const void* s, size_t n) {
  memcpy(p, s, n);
  return p + n;
}

static char* pgw_wstr(char* p, const char* s) {
  return pgw_wmem(p, s, strlen(s) + 1);
}

// A message: its type byte and length word, body bytes to fill after.
static char* pgw_msg(PgwConn* c, char type, uint32_t body) {
  char* p = pgw_put(c, 5 + body);
  p[0]    = type;
  return pgw_w32(p + 1, 4 + body);
}

void pgw_parse(PgwConn* c, const char* name, const char* sql, uint16_t n, const uint32_t* types) {
  char* p = pgw_msg(c, 'P', (uint32_t)(strlen(name) + 1 + strlen(sql) + 1 + 2 + 4u * n));
  p       = pgw_wstr(p, name);
  p       = pgw_wstr(p, sql);
  p       = pgw_w16(p, n);
  for (uint16_t i = 0; i < n; i += 1) {
    p = pgw_w32(p, types[i]);
  }
}

void pgw_bind_exec(PgwConn* c, const char* name, uint16_t n, const char* const* vals, const uint32_t* lens, uint16_t nfmt,
  const uint16_t* fmt) {
  uint32_t body = 1 + (uint32_t)strlen(name) + 1 + 2 + (n > 0 ? 2 : 0) + 2 + 2 + 2u * nfmt;
  for (uint16_t i = 0; i < n; i += 1) {
    body += 4 + lens[i];
  }
  char* p = pgw_msg(c, 'B', body);
  *p++    = 0;  // the unnamed portal
  p       = pgw_wstr(p, name);
  if (n > 0) {
    p = pgw_w16(p, 1);  // one format code for every parameter: binary
    p = pgw_w16(p, 1);
  } else {
    p = pgw_w16(p, 0);
  }
  p = pgw_w16(p, n);
  for (uint16_t i = 0; i < n; i += 1) {
    p = pgw_w32(p, lens[i]);
    p = pgw_wmem(p, vals[i], lens[i]);
  }
  p = pgw_w16(p, nfmt);  // the result-format codes: one for every column, or one per column
  for (uint16_t i = 0; i < nfmt; i += 1) {
    p = pgw_w16(p, fmt[i]);
  }
  p    = pgw_msg(c, 'E', 5);
  *p++ = 0;         // the unnamed portal
  pgw_w32(p, 0);    // no row limit
}

void pgw_sync(PgwConn* c) {
  pgw_msg(c, 'S', 0);
}

int pgw_flush(PgwConn* c) {
  PgwOut* o = &c->out;
  while (o->off < o->n) {
    ssize_t k = send(c->fd, o->p + o->off, o->n - o->off, MSG_NOSIGNAL);
    if (k > 0) {
      o->off += (uint32_t)k;
    } else if (k < 0 && errno == EINTR) {
      continue;
    } else if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return 1;
    } else {
      c->bad = true;
      return pgw_failc(c, "send: %s", k < 0 ? strerror(errno) : "nothing sent");
    }
  }
  o->off = o->n = 0;
  return 0;
}

// ---- the receive buffer ----

uint64_t pgw_in_bytes;

static struct {
  char**   p;  // buffers replaced while pinned, freed at pgw_release
  uint32_t n, cap;
} pgw_old;

static PgwConn* pgw_pins;

static void pgw_retire(char* p) {
  if (pgw_old.n == pgw_old.cap) {
    pgw_old.cap = pgw_old.cap ? pgw_old.cap * 2 : 16;
    pgw_old.p   = pgw_alloc(pgw_old.p, pgw_old.cap * sizeof *pgw_old.p);
  }
  pgw_old.p[pgw_old.n++] = p;
}

void pgw_pin(PgwConn* c) {
  c->in.pinned = true;
  if (!c->pin_listed) {
    c->pin_listed = true;
    c->pin_next   = pgw_pins;
    pgw_pins      = c;
  }
}

void pgw_release(void) {
  for (PgwConn* c = pgw_pins; c != NULL;) {
    PgwConn* next = c->pin_next;
    c->in.pinned  = false;
    c->pin_listed = false;
    c->pin_next   = NULL;
    c = next;
  }
  pgw_pins = NULL;
  for (uint32_t i = 0; i < pgw_old.n; i += 1) {
    free(pgw_old.p[i]);
  }
  pgw_old.n = 0;
}

// At least PGW_ROOM free bytes after tail. Unpinned, the unread bytes move to the start (or the
// buffer grows); pinned, the rows handed out this round point below head, so the unread bytes
// go to a fresh buffer and this one waits for pgw_release.
static void pgw_room(PgwIn* b) {
  if (!b->pinned && b->head == b->tail) {
    b->head = b->scan = b->tail = 0;
  }
  if (b->p != NULL && b->cap - b->tail >= PGW_ROOM) {
    return;
  }
  uint32_t live = b->tail - b->head;
  if (b->p != NULL && !b->pinned) {
    if (b->head > 0) {
      memmove(b->p, b->p + b->head, live);
      b->scan -= b->head;
      b->tail = live;
      b->head = 0;
    }
    if (b->cap - b->tail < PGW_ROOM) {
      uint32_t cap = b->cap * 2;
      while (cap - live < PGW_ROOM) {
        cap *= 2;
      }
      b->p   = pgw_alloc(b->p, cap);
      b->cap = cap;
    }
    return;
  }
  uint32_t cap = b->cap ? b->cap : PGW_FIRST;
  while (cap - live < PGW_ROOM) {
    cap *= 2;
  }
  char* q = pgw_alloc(NULL, cap);
  if (live > 0) {
    memcpy(q, b->p + b->head, live);
  }
  if (b->p != NULL) {
    pgw_retire(b->p);
  }
  b->p      = q;
  b->cap    = cap;
  b->scan  -= b->head;
  b->tail   = live;
  b->head   = 0;
  b->pinned = false;
}

static bool pgw_known(char t) {
  switch (t) {
  case 'R': case 'S': case 'K': case 'Z': case 'E': case 'N': case '1': case '2': case 'C': case 'D': case 'I':
    return true;
  default:
    return false;
  }
}

// Checks the headers that have arrived since the last call and moves scan past each whole
// message (host/pgwire.h).
static int pgw_scan(PgwConn* c) {
  PgwIn* b = &c->in;
  while (b->tail - b->scan >= 5) {
    const char* p = b->p + b->scan;
    uint32_t    n = pgw_be32(p + 1);
    if (!pgw_known(p[0])) {
      return pgw_failc(c, "a message of type 0x%02x, which this client does not frame", (unsigned)(unsigned char)p[0]);
    }
    if (n < 4) {
      return pgw_failc(c, "a message of type '%c' with length %u, under 4", p[0], n);
    }
    if (n > PGW_MAX_MSG) {
      return pgw_failc(c, "a message of type '%c' with length %u, over %u", p[0], n, PGW_MAX_MSG);
    }
    if (b->tail - b->scan - 1 < n) {
      break;
    }
    b->zs   += p[0] == 'Z';
    b->scan += 1 + n;
  }
  return 0;
}

// recv until a call returns less than its room, EAGAIN, or an error (as libpq's pqReadData:
// another only when one filled the room). End of file or an error after data waits for the
// next event, which reports it.
int pgw_recv(PgwConn* c) {
  PgwIn* b   = &c->in;
  bool   got = false;
  for (;;) {
    pgw_room(b);
    uint32_t room = b->cap - b->tail;
    ssize_t  k    = recv(c->fd, b->p + b->tail, room, 0);
    if (k > 0) {
      b->tail      += (uint32_t)k;
      pgw_in_bytes += (uint64_t)k;
      got = true;
      if ((uint32_t)k < room) {
        break;
      }
      continue;
    }
    if (k < 0 && errno == EINTR) {
      continue;
    }
    if ((k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) || got) {
      break;
    }
    c->bad = true;
    return k == 0 ? pgw_failc(c, "the server closed the connection") : pgw_failc(c, "recv: %s", strerror(errno));
  }
  return pgw_scan(c);
}

void pgw_error(PgwConn* c, const PgwMsg* m, char code[6]) {
  const char *sev = NULL, *msg = NULL, *p = m->body, *end = m->body + m->len;
  code[0] = 0;
  while (p < end && *p != 0) {
    char        t = *p++;
    const char* z = memchr(p, 0, (size_t)(end - p));
    if (z == NULL) {
      break;
    }
    if (t == 'V' || (t == 'S' && sev == NULL)) {
      sev = p;
    } else if (t == 'C' && z - p == 5) {
      memcpy(code, p, 6);
    } else if (t == 'M') {
      msg = p;
    }
    p = z + 1;
  }
  snprintf(c->err, sizeof c->err, "%s:  %s%s%s%s", sev != NULL ? sev : "ERROR", msg != NULL ? msg : "",
    code[0] ? " (SQLSTATE " : "", code, code[0] ? ")" : "");
}

// ---- the connect and the startup ----

static int pgw_socket(char* err, size_t errn) {
  int fd = socket(tg.addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    snprintf(err, errn, "socket: %s", strerror(errno));
    return -1;
  }
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
  if (connect(fd, (const struct sockaddr*)&tg.addr, tg.alen) < 0 && errno != EINPROGRESS) {
    snprintf(err, errn, "connect: %s", strerror(errno));
    close(fd);
    return -1;
  }
  return fd;
}

int pgw_open(PgwConn* c) {
  c->phase  = PH_CONNECT;
  c->sasl   = 0;
  c->authed = false;
  c->bad    = false;
  c->keyed  = false;
  c->pid    = 0;
  c->key    = 0;
  c->err[0] = 0;
  c->in.head = c->in.scan = c->in.tail = c->in.zs = 0;
  c->out.n = c->out.off = 0;
  c->fd = pgw_socket(c->err, sizeof c->err);
  return c->fd < 0 ? -1 : 0;
}

uint32_t pgw_events(const PgwConn* c) {
  return c->phase == PH_CONNECT ? EPOLLOUT : EPOLLIN | (c->out.off < c->out.n ? EPOLLOUT : 0);
}

static void pgw_hello(PgwConn* c) {
  static const char* const own[] = { "client_encoding", "UTF8", NULL };
  uint32_t                 body  = 4 + 5 + (uint32_t)strlen(tg.user) + 1 + 9 + (uint32_t)strlen(tg.db) + 1 + 1;
  for (int k = 0; k < 2; k += 1) {
    const char* const* kv = k == 0 ? own : tg.params;
    for (int i = 0; kv != NULL && kv[i] != NULL; i += 2) {
      body += (uint32_t)(strlen(kv[i]) + 1 + strlen(kv[i + 1]) + 1);
    }
  }
  char* p = pgw_put(c, 4 + body);  // a StartupMessage has no type byte
  p       = pgw_w32(p, 4 + body);
  p       = pgw_w32(p, 196608);  // protocol 3.0
  p       = pgw_wstr(p, "user");
  p       = pgw_wstr(p, tg.user);
  p       = pgw_wstr(p, "database");
  p       = pgw_wstr(p, tg.db);
  for (int k = 0; k < 2; k += 1) {
    const char* const* kv = k == 0 ? own : tg.params;
    for (int i = 0; kv != NULL && kv[i] != NULL; i += 2) {
      p = pgw_wstr(p, kv[i]);
      p = pgw_wstr(p, kv[i + 1]);
    }
  }
  *p = 0;
}

// AuthenticationSASL: the mechanisms offered; SCRAM-SHA-256 must be one. Sends client-first.
static int pgw_sasl_start(PgwConn* c, const char* d, uint32_t n) {
  bool found = false;
  while (n > 0 && d[0] != 0) {
    size_t k = strnlen(d, n);
    if (k == n) {
      return pgw_failc(c, "AuthenticationSASL with an unterminated mechanism");
    }
    found |= k == 13 && !memcmp(d, "SCRAM-SHA-256", 13);
    d += k + 1;
    n -= (uint32_t)(k + 1);
  }
  if (c->sasl != 0) {
    return pgw_failc(c, "a second AuthenticationSASL");
  }
  if (!found) {
    return pgw_failc(c, "the server offers no SCRAM-SHA-256 (channel binding is not supported)");
  }
  if (tg.pass == NULL) {
    return pgw_failc(c, "the server asks for SCRAM-SHA-256 and DATABASE_URL has no password");
  }
  unsigned char r[18];
  if (getrandom(r, sizeof r, 0) != (ssize_t)sizeof r) {
    return pgw_failc(c, "getrandom: %s", strerror(errno));
  }
  pgw_b64(r, sizeof r, c->nonce);
  char first[40];
  int  fn = snprintf(first, sizeof first, "n,,n=,r=%s", c->nonce);
  char* p = pgw_msg(c, 'p', 14 + 4 + (uint32_t)fn);
  p       = pgw_wstr(p, "SCRAM-SHA-256");
  p       = pgw_w32(p, (uint32_t)fn);
  pgw_wmem(p, first, (size_t)fn);
  c->sasl = 1;
  return PGW_WAIT;
}

// AuthenticationSASLContinue: server-first "r=NONCE,s=SALT,i=COUNT[,extensions]". Sends
// client-final with the proof and keeps the ServerSignature the final message must carry.
static int pgw_sasl_continue(PgwConn* c, const char* d, uint32_t n) {
  if (c->sasl != 1) {
    return pgw_failc(c, "AuthenticationSASLContinue out of place");
  }
  const char* at[3];
  size_t      len[3];
  const char* p   = d;
  const char* end = d + n;
  for (int k = 0; k < 3; k += 1) {
    const char* comma = memchr(p, ',', (size_t)(end - p));
    const char* stop  = comma != NULL ? comma : end;
    if (stop - p < 2 || p[0] != "rsi"[k] || p[1] != '=') {
      return pgw_failc(c, "SCRAM: the server-first message is not r=..,s=..,i=..");
    }
    at[k]  = p + 2;
    len[k] = (size_t)(stop - p - 2);
    p      = comma != NULL ? comma + 1 : end;
  }
  size_t cn = strlen(c->nonce);
  if (len[0] <= cn || memcmp(at[0], c->nonce, cn) != 0) {
    return pgw_failc(c, "SCRAM: the server's nonce does not extend the client's");
  }
  for (size_t i = 0; i < len[0]; i += 1) {
    if (at[0][i] < 0x21 || at[0][i] > 0x7e) {
      return pgw_failc(c, "SCRAM: the server's nonce has a character outside printable ASCII");
    }
  }
  unsigned char salt[PGW_SALT];
  long          sn = pgw_unb64(at[1], len[1], salt, sizeof salt);
  if (sn <= 0) {
    return pgw_failc(c, "SCRAM: the salt is not base64 of 1 to %u bytes", PGW_SALT);
  }
  uint64_t iter = 0;  // stops growing past the cap, so a long count cannot overflow it
  for (size_t i = 0; i < len[2]; i += 1) {
    if (at[2][i] < '0' || at[2][i] > '9') {
      return pgw_failc(c, "SCRAM: the iteration count is not a number");
    }
    iter = iter > PGW_ITER_MAX ? iter : iter * 10 + (uint64_t)(at[2][i] - '0');
  }
  if (len[2] == 0 || iter < 1) {
    return pgw_failc(c, "SCRAM: the iteration count is empty or 0");
  }
  if (iter > PGW_ITER_MAX) {
    return pgw_failc(c, "SCRAM: an iteration count over %u, which this client refuses (the salting runs on the event loop; PostgreSQL's default is 4096)", PGW_ITER_MAX);
  }
  unsigned char sp[32], ck[32], sk[32], stored[32], csig[32], proof[32];
  pgw_salted(salt, (uint32_t)sn, (uint32_t)iter, sp);
  PgwMac k;
  pgw_mac_key(&k, sp, sizeof sp);
  pgw_mac(&k, "Client Key", 10, ck);
  pgw_mac(&k, "Server Key", 10, sk);
  SHA256(ck, sizeof ck, stored);
  // AuthMessage = client-first-bare "," server-first "," client-final-without-proof
  size_t bn = 5 + cn, wn = 9 + len[0];
  size_t an = bn + 1 + n + 1 + wn;
  char*  am = pgw_alloc(NULL, an);
  char*  q  = am;
  q         = pgw_wmem(q, "n=,r=", 5);
  q         = pgw_wmem(q, c->nonce, cn);
  *q++      = ',';
  q         = pgw_wmem(q, d, n);
  *q++      = ',';
  q         = pgw_wmem(q, "c=biws,r=", 9);  // biws: base64 of the gs2 header "n,,"
  pgw_wmem(q, at[0], len[0]);
  pgw_mac_key(&k, stored, sizeof stored);
  pgw_mac(&k, am, an, csig);
  for (int i = 0; i < 32; i += 1) {
    proof[i] = ck[i] ^ csig[i];
  }
  pgw_mac_key(&k, sk, sizeof sk);
  pgw_mac(&k, am, an, c->sig);
  free(am);
  char pb[48];
  size_t pn = pgw_b64(proof, sizeof proof, pb);
  char*  w  = pgw_msg(c, 'p', (uint32_t)(wn + 3 + pn));
  w         = pgw_wmem(w, "c=biws,r=", 9);
  w         = pgw_wmem(w, at[0], len[0]);
  w         = pgw_wmem(w, ",p=", 3);
  pgw_wmem(w, pb, pn);
  OPENSSL_cleanse(sp, sizeof sp);
  OPENSSL_cleanse(ck, sizeof ck);
  OPENSSL_cleanse(sk, sizeof sk);
  OPENSSL_cleanse(stored, sizeof stored);
  OPENSSL_cleanse(&k, sizeof k);
  c->sasl = 2;
  return PGW_WAIT;
}

// AuthenticationSASLFinal: "v=SIGNATURE[,extensions]", which must be the ServerSignature.
static int pgw_sasl_final(PgwConn* c, const char* d, uint32_t n) {
  if (c->sasl != 2) {
    return pgw_failc(c, "AuthenticationSASLFinal out of place");
  }
  if (n >= 2 && d[0] == 'e' && d[1] == '=') {
    return pgw_failc(c, "SCRAM: the server refused: %.*s", (int)(n - 2 < 100 ? n - 2 : 100), d + 2);
  }
  const char*   comma = memchr(d, ',', n);
  size_t        vn    = (size_t)((comma != NULL ? comma : d + n) - d);
  unsigned char v[32];
  if (vn < 2 || d[0] != 'v' || d[1] != '=' || pgw_unb64(d + 2, vn - 2, v, sizeof v) != 32
    || CRYPTO_memcmp(v, c->sig, 32) != 0) {
    return pgw_failc(c, "SCRAM: the server's signature does not verify");
  }
  c->sasl = 3;
  return PGW_WAIT;
}

static int pgw_auth(PgwConn* c, const PgwMsg* m) {
  if (m->len < 4) {
    return pgw_failc(c, "an Authentication message of %u bytes", m->len);
  }
  uint32_t    code = pgw_be32(m->body);
  const char* d    = m->body + 4;
  uint32_t    n    = m->len - 4;
  switch (code) {
  case 0:
    if (c->sasl == 1 || c->sasl == 2) {
      return pgw_failc(c, "AuthenticationOk before the SCRAM exchange completed");
    }
    c->authed = true;
    return PGW_WAIT;
  case 3: {
    if (tg.pass == NULL) {
      return pgw_failc(c, "the server asks for a password and DATABASE_URL has none");
    }
    char* p = pgw_msg(c, 'p', (uint32_t)strlen(tg.pass) + 1);
    pgw_wstr(p, tg.pass);
    return PGW_WAIT;
  }
  case 10:
    return pgw_sasl_start(c, d, n);
  case 11:
    return pgw_sasl_continue(c, d, n);
  case 12:
    return pgw_sasl_final(c, d, n);
  default:
    return pgw_failc(c, "authentication method %u is not supported (trust, password and SCRAM-SHA-256 are)", code);
  }
}

// One startup message: PGW_WAIT for more, PGW_DONE at ReadyForQuery, or PGW_FAIL.
static int pgw_hello_reply(PgwConn* c, const PgwMsg* m) {
  switch (m->type) {
  case 'R':
    return pgw_auth(c, m);
  case 'S': {
    const char* z = memchr(m->body, 0, m->len);
    if (z == NULL || memchr(z + 1, 0, m->len - (uint32_t)(z + 1 - m->body)) == NULL) {
      return pgw_failc(c, "a ParameterStatus that is not two strings");
    }
    if (!strcmp(m->body, "integer_datetimes") && strcmp(z + 1, "on") != 0) {
      return pgw_failc(c, "integer_datetimes is %s: binary timestamps would not be 8-byte integers", z + 1);
    }
    return PGW_WAIT;
  }
  case 'K':
    if (m->len != 8) {
      return pgw_failc(c, "BackendKeyData of %u bytes (protocol 3.0 has 8)", m->len);
    }
    c->pid   = (int32_t)pgw_be32(m->body);
    c->key   = (int32_t)pgw_be32(m->body + 4);
    c->keyed = true;
    return PGW_WAIT;
  case 'N':
    return PGW_WAIT;
  case 'E': {
    char code[6];
    pgw_error(c, m, code);
    return PGW_FAIL;
  }
  case 'Z':
    if (!c->authed || m->len != 1) {
      return pgw_failc(c, "ReadyForQuery before AuthenticationOk");
    }
    c->phase = PH_UP;
    return PGW_DONE;
  default:
    return pgw_failc(c, "a message of type '%c' during the startup", m->type);
  }
}

int pgw_startup(PgwConn* c, uint32_t ev) {
  if (c->phase == PH_CONNECT) {
    if (!(ev & (EPOLLOUT | EPOLLERR | EPOLLHUP))) {
      return PGW_WAIT;
    }
    int       e = 0;
    socklen_t n = sizeof e;
    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &e, &n) < 0) {
      e = errno;
    }
    if (e != 0) {
      c->bad = true;
      return pgw_failc(c, "connect: %s", strerror(e));
    }
    pgw_hello(c);
    c->phase = PH_STARTUP;
    ev       = 0;
  }
  if ((ev & (EPOLLIN | EPOLLERR | EPOLLHUP)) && pgw_recv(c) < 0) {
    return PGW_FAIL;
  }
  PgwMsg m;
  while (c->phase == PH_STARTUP && pgw_take(c, &m)) {
    int r = pgw_hello_reply(c, &m);
    if (r != PGW_WAIT) {
      OPENSSL_cleanse(c->sig, sizeof c->sig);
      return r;
    }
  }
  return pgw_flush(c) < 0 ? PGW_FAIL : PGW_WAIT;
}

void pgw_close(PgwConn* c) {
  if (c->fd >= 0) {
    close(c->fd);
  }
  c->fd = -1;
  if (c->in.pinned) {
    pgw_retire(c->in.p);
    c->in.p      = NULL;
    c->in.cap    = 0;
    c->in.pinned = false;
  }
  c->in.head = c->in.scan = c->in.tail = c->in.zs = 0;
  c->out.n = c->out.off = 0;
  c->phase = PH_CONNECT;
  c->sasl  = 0;
  OPENSSL_cleanse(c->sig, sizeof c->sig);
}

// ---- the cancel ----

int pgw_cancel_open(PgwCancel* x, const PgwConn* c) {
  x->phase  = 0;
  x->sent   = 0;
  x->err[0] = 0;
  char* p   = pgw_w32(x->req, 16);
  p         = pgw_w32(p, 80877102);
  p         = pgw_w32(p, (uint32_t)c->pid);
  pgw_w32(p, (uint32_t)c->key);
  x->fd = pgw_socket(x->err, sizeof x->err);
  return x->fd < 0 ? -1 : 0;
}

uint32_t pgw_cancel_events(const PgwCancel* x) {
  return x->phase < 2 ? EPOLLOUT : EPOLLIN;
}

int pgw_cancel_poll(PgwCancel* x, uint32_t ev) {
  if (x->phase == 0) {
    if (!(ev & (EPOLLOUT | EPOLLERR | EPOLLHUP))) {
      return PGW_WAIT;
    }
    int       e = 0;
    socklen_t n = sizeof e;
    if (getsockopt(x->fd, SOL_SOCKET, SO_ERROR, &e, &n) < 0) {
      e = errno;
    }
    if (e != 0) {
      snprintf(x->err, sizeof x->err, "connect: %s", strerror(e));
      return PGW_FAIL;
    }
    x->phase = 1;
  }
  if (x->phase == 1) {
    while (x->sent < sizeof x->req) {
      ssize_t k = send(x->fd, x->req + x->sent, sizeof x->req - x->sent, MSG_NOSIGNAL);
      if (k > 0) {
        x->sent += (uint8_t)k;
      } else if (k < 0 && errno == EINTR) {
        continue;
      } else if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return PGW_WAIT;
      } else {
        snprintf(x->err, sizeof x->err, "send: %s", k < 0 ? strerror(errno) : "nothing sent");
        return PGW_FAIL;
      }
    }
    x->phase = 2;
    return PGW_WAIT;
  }
  char t[256];
  for (;;) {
    ssize_t k = recv(x->fd, t, sizeof t, 0);
    if (k > 0) {
      pgw_in_bytes += (uint64_t)k;
      continue;
    }
    if (k < 0 && errno == EINTR) {
      continue;
    }
    if (k == 0) {
      return PGW_DONE;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return PGW_WAIT;
    }
    snprintf(x->err, sizeof x->err, "recv: %s", strerror(errno));
    return PGW_FAIL;
  }
}

void pgw_cancel_close(PgwCancel* x) {
  if (x->fd >= 0) {
    close(x->fd);
  }
  x->fd = -1;
}
