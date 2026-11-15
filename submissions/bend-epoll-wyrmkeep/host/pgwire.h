// The PostgreSQL wire client under host/pg.c (docs/specs/S25-the-framer.md): protocol 3.0
// over one non-blocking TCP socket per connection, written for route A′'s pool and nothing
// else. host/pgwire.c holds the connect, the startup and its authentication (trust, cleartext,
// SCRAM-SHA-256 without channel binding), the message encoders, the receive buffer and the
// CancelRequest; this header adds the framing helpers host/pg.c calls per message and per row.
// Outside every proof, like the rest of the host's C.
//
// Not implemented: SSL, GSS, channel binding, SASLprep, MD5, COPY, NotificationResponse,
// FunctionCall, protocol 3.2 (longer cancel keys, NegotiateProtocolVersion), a second host or
// address, Unix sockets. A server that asks for any of them fails the connection.
//
// The receive buffer. recv appends at tail; pgw_recv then checks each new message's header
// (its type byte is one the client knows, its length is at least 4 and at most PGW_MAX_MSG)
// and moves scan past each message that has arrived whole, so [head, scan) holds whole,
// checked messages and zs counts the ReadyForQuery messages among them. pgw_take hands the
// message at head over and moves head past it. Bytes before head stay where they are until
// the next recv makes room: a row handed to the policy is a view into them (host/pg.c,
// pg_row_put), so once pg.c pins the buffer (pgw_pin) nothing in it moves or is freed until
// pgw_release, the round's end (host/pg.c, pg_release). A pinned buffer that runs out of room
// is replaced by a fresh one holding the unread bytes, and is freed at pgw_release.
#ifndef BM_PGWIRE_H
#define BM_PGWIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PGW_MAX_MSG (64u << 20)  // a longer message fails the connection

enum { PGW_FAIL = -1, PGW_WAIT = 0, PGW_DONE = 1 };  // pgw_startup and pgw_cancel_poll

typedef struct {
  char*    p;
  uint32_t cap, head, scan, tail;
  uint32_t zs;      // ReadyForQuery messages in [head, scan)
  bool     pinned;  // rows handed out this round point into p (pgw_pin)
} PgwIn;

typedef struct {
  char*    p;
  uint32_t cap, n, off;  // [off, n) not sent yet
} PgwOut;

typedef struct PgwConn {
  int      fd;      // -1: none
  uint8_t  phase;   // pgwire.c: connecting, startup, up
  uint8_t  sasl;    // SCRAM: 0 none, 1 client-first sent, 2 client-final sent, 3 server verified
  bool     authed;  // AuthenticationOk arrived
  bool     bad;     // end of file or a socket error: close it, do not shut it down
  bool     keyed;   // BackendKeyData arrived
  int32_t  pid, key;
  PgwIn    in;
  PgwOut   out;
  char     nonce[25];          // SCRAM: the client nonce, base64 of 18 random bytes
  unsigned char sig[32];       // SCRAM: the ServerSignature the server must send back
  struct PgwConn* pin_next;    // the pinned list (pgw_pin, pgw_release)
  bool     pin_listed;         // on it
  char     err[256];           // why it failed, or the server's last ErrorResponse
} PgwConn;

typedef struct {
  int     fd;  // -1: none
  uint8_t phase;
  uint8_t sent;      // bytes of req sent
  char    req[16];   // the CancelRequest: length, code 80877102, pid, key
  char    err[160];
} PgwCancel;

typedef struct {
  char        type;
  const char* body;  // after the length word
  uint32_t    len;   // of body
} PgwMsg;

typedef struct {
  const char* p;
  int32_t     n;  // -1: NULL
} PgwCol;

// DATABASE_URL (postgres:// or postgresql://, user, password, host, port, dbname, and the query
// parameters sslmode=disable|allow|prefer, gssencmode=disable|prefer, connect_timeout,
// application_name and options, the last three ignored) parsed and its host resolved, once per
// process; params (key, value, ..., NULL) are the StartupMessage's parameters after user and
// database. 0, or -1 with the reason in err.
int pgw_target(const char* url, const char* const* params, char* err, size_t errn);

int  pgw_open(PgwConn* c);                  // a socket and a non-blocking connect: 0, or -1 (c->err)
int  pgw_startup(PgwConn* c, uint32_t ev);  // after an epoll event: PGW_WAIT, PGW_DONE (ReadyForQuery) or PGW_FAIL
uint32_t pgw_events(const PgwConn* c);      // the epoll events the connection waits for now
void pgw_close(PgwConn* c);                 // closes the socket; a pinned buffer waits for pgw_release

void pgw_parse(PgwConn* c, const char* name, const char* sql, uint16_t n, const uint32_t* types);
// Bind of the unnamed portal to statement name, every parameter binary, then Execute with no
// row limit. fmt holds the Bind's nfmt result-format codes as the protocol counts them (0 text,
// 1 binary): one code stands for every column, more are one per column (host/sql.h has the
// two lists host/pg.c sends).
void pgw_bind_exec(PgwConn* c, const char* name, uint16_t n, const char* const* vals, const uint32_t* lens, uint16_t nfmt,
  const uint16_t* fmt);
void pgw_sync(PgwConn* c);
int  pgw_flush(PgwConn* c);  // 0 all sent, 1 bytes left (wait for EPOLLOUT), -1 failed (c->err)
int  pgw_recv(PgwConn* c);   // 0, or -1: end of file, a socket error or a header that cannot be framed (c->err)

// The bytes every recv on a database socket has returned in this process, so it only grows:
// pgw_recv (a connection's startup and its replies), pgw_cancel_poll, and host/pg.c's pg_drain,
// which reads what a connection it gave up still sends and adds those bytes here.
extern uint64_t pgw_in_bytes;

// An ErrorResponse's SQLSTATE into code (5 characters and a NUL, "" if absent); its severity,
// message and code into c->err for the slot-down line.
void pgw_error(PgwConn* c, const PgwMsg* m, char code[6]);

void pgw_pin(PgwConn* c);  // rows handed out point into c's receive buffer until pgw_release
void pgw_release(void);    // the round's rows are gone: unpin every buffer, free the replaced ones

int  pgw_cancel_open(PgwCancel* x, const PgwConn* c);  // CancelRequest for c's backend: 0, or -1 (x->err)
int  pgw_cancel_poll(PgwCancel* x, uint32_t ev);       // PGW_WAIT, PGW_DONE (the server closed) or PGW_FAIL
uint32_t pgw_cancel_events(const PgwCancel* x);
void pgw_cancel_close(PgwCancel* x);

static inline uint32_t pgw_be32(const char* p) {
  const unsigned char* u = (const unsigned char*)p;
  return (uint32_t)u[0] << 24 | (uint32_t)u[1] << 16 | (uint32_t)u[2] << 8 | u[3];
}

static inline uint16_t pgw_be16(const char* p) {
  const unsigned char* u = (const unsigned char*)p;
  return (uint16_t)(u[0] << 8 | u[1]);
}

static inline uint64_t pgw_be64(const char* p) {
  return (uint64_t)pgw_be32(p) << 32 | pgw_be32(p + 4);
}

// The whole message at head, if one has arrived: m points into the buffer, head moves past it.
static inline bool pgw_take(PgwConn* c, PgwMsg* m) {
  PgwIn* b = &c->in;
  if (b->head == b->scan) {
    return false;
  }
  const char* p = b->p + b->head;
  uint32_t    n = pgw_be32(p + 1);
  m->type = p[0];
  m->body = p + 5;
  m->len  = n - 4;
  b->head += 1 + n;
  b->zs   -= m->type == 'Z';
  return true;
}

// A DataRow's body split into its n columns: true when it holds exactly n and their lengths
// fill it exactly.
static inline bool pgw_cols(const char* body, uint32_t len, PgwCol* c, uint16_t n) {
  if (len < 2 || pgw_be16(body) != n) {
    return false;
  }
  uint32_t at = 2;
  for (uint16_t j = 0; j < n; j += 1) {
    if (len - at < 4) {
      return false;
    }
    int32_t k = (int32_t)pgw_be32(body + at);
    at += 4;
    if (k < -1 || (k > 0 && (uint32_t)k > len - at)) {
      return false;
    }
    c[j].p = body + at;
    c[j].n = k;
    at += k > 0 ? (uint32_t)k : 0;
  }
  return at == len;
}

#endif
