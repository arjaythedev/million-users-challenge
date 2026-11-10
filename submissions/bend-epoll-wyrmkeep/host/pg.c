// The C side of host/pg.bend: route A′'s database connections
// (docs/specs/S08-pg-effect-and-interpreter.md, docs/specs/S10-two-batches.md,
// docs/specs/S15-connections-by-kind.md, docs/specs/S25-the-framer.md; notes in
// .local/receipts/S08/notes.md, .local/receipts/S10/notes.md, .local/receipts/S15/notes.md
// and .local/receipts/S25/notes.md). Spliced after the runtime with host/epoll.c;
// host/host.h is what the two share. The wire protocol is host/pgwire.c's, a unit of its
// own (host/pgwire.h). Outside every proof.
//
// A pool of --pool connections, each a non-blocking socket of host/pgwire.c's in the
// reactor's epoll sets with HOST_TAG_PG | slot, connected and logged in from the reactor
// (SLOT_CONNECTING). The five statements of host/sql.h are prepared once per connection
// (Parse each, one Sync); nothing else is sent.
//
// Two batches are open at once, one of reads (SelectOne, FeedQuery,
// PostQuery) and one of writes (InsertPost, InsertLike), as the kernel's
// Open{b, kind} names them. Each is a list of statements held here until it
// is flushed, at the start of a round (pg_tick): when it holds --batch-max
// statements, or after its first Queue by --batch-wait µs for the read batch
// and --write-wait µs for the write batch, so that more writes share one
// commit; each kind has its own count and clock. With --window-us, a turn that
// is about to sleep flushes the read batch first, whatever its clock
// (pg_flush_reads; S21). The flush writes every
// statement as Bind and Execute (binary parameters and results, no Describe) and one Sync
// after the last (one implicit transaction), as one buffer, to a ready connection of the
// batch's kind: the last
// --write-slots slots of the pool take write batches only, the others read
// batches only, so a read never waits behind a write's commit; of those, the
// one with the fewest batches in flight (pg_pick). --write-slots 0 is the pick
// before S15: any slot, a read preferring one that carries no write. The flush
// records Flushed{b}. A connection carries a FIFO of flushed batches. A batch's
// replies stay in the connection's receive buffer until the ReadyForQuery its Sync
// brings has arrived; then pg_frame frames them all, and Synced{b, ok, outs, uptime_s}
// follows, its rows views into that buffer.
//
// A write the database refuses with an integrity error (SQLSTATE class 23: a
// like of a missing post, a token whose user has no row) ends the attempt: the
// server skips everything to the Sync and rolls the whole attempt back. The batch then
// goes again, the same id on the same connection, without the statements
// refused so far; each attempt refuses at least one more, so a batch of n
// statements goes at most n times. The Synced comes from the attempt that
// was not aborted: ok true exactly when every statement it sent answered
// CommandComplete, each outcome from its own replies, a
// refused like NotFound and a refused create Unknown (the status byte reads
// 'I' after a rollback too: S08 notes, premise 1; so it tells nothing of the
// outcome, and any other status after a Sync fails the connection, pg_frame).
// Any other failure, and any failure in a read batch, fails the batch (ok false).
//
// A connection that fails (EOF, a read or write error, replies that cannot be framed:
// pg_frame and host/pgwire.c fail closed) or whose oldest batch waits past --db-timeout is
// given up: every
// batch it carries is Died{b} (a Died batch may have committed, when its Sync
// reached the server), its socket is shut for writing, a cancel goes to its
// backend, and the slot stays SLOT_CLOSING until the server closes the socket,
// so a slot never has two backends; then it reconnects with a back-off. At
// most one cancel connection is in flight, for at most --db-timeout: a slot whose cancel
// would be the
// second waits for the first to end (the trace records each wait), so the
// server sees at most --pool + 1 connections from this process. Each
// connection asks its backend to check every second for a client that has
// gone (client_connection_check_interval), so a backend ends while it waits on
// a lock even when the cancel is lost or still waiting. A batch that finds no
// ready connection of its kind for --db-timeout is Flushed{b} and then Died{b}
// without being sent, whatever the other kind's connections do. A slot keeps
// its kind across reconnects: the kind is its index.
//
// --sync-max K bounds the Synced events of a turn (S19,
// docs/specs/S19-the-round-budget.md). Once K are out, pg_results stops a
// ready slot before its next whole batch of replies, and what it leaves stays in the
// receive buffer. pgw_recv has already moved those bytes off the socket, so
// epoll_pwait2 will not report the slot again: it is marked pending, the turn
// does not sleep (pg_pending), and the next turn's start takes the pending
// slots first (pg_resume), from the one after the slot that spent the last
// budget, under the new budget. A canned batch flushed after the budget ran
// out waits in pg.deferred the same way. Prepares, Died and replies short of
// a ReadyForQuery are not counted: they emit no Synced. A pending slot's batches are
// not timed out while they wait for this host (pg_scan). --read-batch-max N
// flushes the read batch at N statements while the write batch keeps
// --batch-max (pg_cap).
//
// --db canned: no connection. A flushed batch is Synced at once with fixed
// outcomes from the golden files (bench/servers/bend-epoll/README.md).

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include "host.h"
#include "pgwire.h"
#include "sql.h"

enum { SLOT_DOWN, SLOT_CONNECTING, SLOT_PREPARING, SLOT_READY, SLOT_CLOSING };
enum { K_READ, K_WRITE };     // a batch's kind, as Pg.open_batch numbers it

enum { OUT_COMMITTED, OUT_LIKED, OUT_POST, OUT_POSTS, OUT_PING, OUT_NOT_FOUND, OUT_UNKNOWN, OUT_FAILED };

#define PG_BACKOFF_MIN_MS 100u
#define PG_BACKOFF_MAX_MS 2000u
#define PG_CANCEL (1ull << 16)  // epoll data of a slot's cancel socket, beside HOST_TAG_PG | slot

typedef struct {
  uint32_t    req, uid, pid;
  uint8_t     kind;
  bool        refused;  // an integrity error (class 23) refused it; not sent again
  bool        ok;       // this attempt: its CommandComplete came (pg_frame)
  int8_t      tag;      // InsertLike: the rows its CommandComplete counts, 0 or 1; else -1
  char*       text;     // InsertPost's body, else NULL
  uint32_t    tlen;
  const char* at;       // this attempt: its first reply after BindComplete, in the receive buffer
  uint32_t    nrows;    // this attempt: its DataRow messages
} PgStmt;

typedef struct PgBatch {
  uint32_t        id, n, cap;
  uint8_t         kind;     // K_READ or K_WRITE
  PgStmt*         st;
  uint64_t        sent;     // this attempt went out
  bool            failed;   // a failure other than a refusal: the batch fails
  bool            refusal;  // this attempt had a statement refused, and the rest aborted
  struct PgBatch* link;
} PgBatch;

typedef struct {
  PgwConn   w;         // the connection (host/pgwire.h); w.fd -1: none
  PgwCancel cx;        // SLOT_CLOSING: the cancel in flight (cx.fd >= 0), else cx.fd -1
  uint8_t   state;
  bool      out;       // EPOLLOUT wanted: the send buffer holds bytes not sent
  bool      cwait;     // SLOT_CLOSING: its cancel waits for the one in flight
  uint64_t  since;     // connect, prepare or SLOT_CLOSING started
  uint64_t  csince;    // its cancel started
  uint64_t  retry;     // SLOT_DOWN: reconnect at
  uint32_t  backoff;   // ms
  uint32_t  inflight;
  uint32_t  writes;    // of them, write batches
  PgBatch*  head;
  PgBatch*  tail;
  uint32_t  next;      // SLOT_PREPARING: ParseComplete messages so far
  bool      failed;    // SLOT_PREPARING: an ErrorResponse came
  bool      pending;   // --sync-max stopped it with a batch's whole replies in its receive buffer (pg_resume)
} PgSlot;

typedef struct {
  uint32_t id, likes;
  char*    body;
  char*    ts;
  char*    author;
  uint32_t bn, tn, an;
} PgRow;

// An open batch: its statements, NULL while it has none.
typedef struct {
  PgBatch* b;
  uint32_t id;
  bool     ok;       // Queue may target id (false between its Flushed and the next Open of the kind)
  uint64_t first;    // when its first statement was queued
  bool     stalled;  // it is due and no connection of its kind is ready
} PgOpen;

static struct {
  bool     started;
  PgSlot*  slot;
  uint32_t nslot;
  uint32_t pick[2];   // where pg_pick's scan starts, per kind (K_READ's for both at --write-slots 0)
  PgOpen   open[2];   // K_READ, K_WRITE; state0() opens 0 and 1
  uint32_t expect;    // the id the next Open must carry
  uint32_t cancels;   // cancel connections in flight, 0 or 1
  uint32_t turn_synced;  // Synced events emitted this turn (--sync-max)
  uint32_t pending;      // slots marked pending
  uint32_t rot;          // the slot whose Synced spent the last budget: pg_resume starts after it
  PgBatch* deferred;     // --db canned: batches Flushed after the budget ran out, oldest first
  PgBatch* deferred_tail;
  uint64_t scan_at;   // the next slot deadline
  PgBatch* spare;
  // --db canned
  PgRow    feed[20];
  uint32_t nfeed;
  PgRow    post[3];   // ids 1, 250000, 500000
  uint32_t next_post;
  uint64_t flushed, flushed_writes, synced, failed, died, stmts, syncs, resent, refused, cancel_waits;
  uint64_t stops;  // --sync-max: times a connection was stopped with answers left in its receive buffer
  uint64_t held;   // --sync-max, --db canned: batches whose Synced went to a later turn
} pg = { .open = { { .id = 0, .ok = true }, { .id = 1, .ok = true } }, .expect = 2, .scan_at = UINT64_MAX };

static JBuf pg_name, pg_text, pg_ts;

// ---- rows behind handles (docs/specs/S09-rows-as-handles.md) ----
// The rows this round's Synced events named: handles base .. base + n - 1.
// Their body and author point into the receive buffers of the connections
// that brought them, each pinned (pgw_pin) so that nothing in it moves or is
// freed (canned rows point into pg.feed and pg.post, which live as long as the
// process). pg_release, first thing in each Epoll.turn, retires them all and
// unpins the buffers (pgw_release).
static struct {
  HostRow* row;
  uint32_t n, cap, base;
} held;

static void* pg_grow(void* p, uint32_t* cap, size_t size) {
  uint32_t c = *cap ? *cap * 2 : 256;
  void*    q = realloc(p, (size_t)c * size);
  if (q == NULL) {
    abort();
  }
  *cap = c;
  return q;
}

static uint32_t pg_hold(uint32_t req, uint32_t id, uint32_t likes, const char* body, size_t bn, const char* ts, size_t tn,
  const char* author, size_t an) {
  if (held.n == held.cap) {
    held.row = pg_grow(held.row, &held.cap, sizeof *held.row);
  }
  HostRow* w = &held.row[held.n];
  if (tn > sizeof w->ts) {
    abort();  // pg_iso writes at most 27 bytes; pg_canned_load refuses longer
  }
  w->req    = req;
  w->id     = id;
  w->likes  = likes;
  w->body   = body;
  w->bn     = (uint32_t)bn;
  w->author = author;
  w->an     = (uint32_t)an;
  w->tn     = (uint32_t)tn;
  memcpy(w->ts, ts, tn);
  return held.base + held.n++;
}

const HostRow* pg_row_at(uint32_t h) {
  uint32_t k = h - held.base;  // mod 2^32: retired and future handles land past n
  return k < held.n ? &held.row[k] : NULL;
}

const HostRow* pg_row(uint32_t h, uint32_t req, uint32_t id, const char** why) {
  const HostRow* w = pg_row_at(h);
  if (w == NULL) {
    *why = "not a row of this round";
  } else if (w->req != req) {
    *why = "issued for another request";
    w    = NULL;
  } else if (w->id != id) {
    *why = "names another post than the id beside it";
    w    = NULL;
  }
  return w;
}

const HostRow* pg_run(uint32_t h, uint32_t n, uint32_t req, const char** why, uint32_t* at) {
  uint32_t k = h - held.base;  // mod 2^32, as pg_row_at
  *at = h;
  if (n == 0) {
    *why = "a run of no rows";
    return NULL;
  }
  for (uint32_t i = 0; i < n; i += 1) {
    if (k + i >= held.n) {  // k itself past n when h is retired or yet to come
      *at  = h + i;
      *why = "not a row of this round";
      return NULL;
    }
    if (held.row[k + i].req != req) {
      *at  = h + i;
      *why = "issued for another request";
      return NULL;
    }
  }
  // pg_outcome holds an answer's rows as neighbours and pg_apart keeps two answers of one
  // request from lying side by side, so a row of req next to the run is a row of the same
  // answer that the run leaves out
  if (k > 0 && held.row[k - 1].req == req) {
    *at  = h - 1;
    *why = "the run starts after its answer's first row";
    return NULL;
  }
  if (k + n < held.n && held.row[k + n].req == req) {
    *at  = h + n;
    *why = "the run ends before its answer's last row";
    return NULL;
  }
  return &held.row[k];
}

// --rows block, before an answer's first row is held: pg_run takes a stretch of req's rows
// with no row of req beside it for one statement's whole answer, so a second answer of the
// request must not begin right after its first. The policy queues one statement per request
// (src/policy.bend, arrived.commands) and request numbers are unique (A6.11); this refuses
// the case where that fails instead of resting on it.
static void pg_apart(uint32_t req) {
  if (held.n > 0 && held.row[held.n - 1].req == req) {
    fprintf(stderr, "bend-epoll: Synced: req %u: a second answer's rows right after its first\n", req);
    exit(70);
  }
}

void pg_release(void) {
  pgw_release();
  held.base += held.n;
  held.n     = 0;
}

static uint64_t pg_ms(uint32_t ms) {
  return (uint64_t)ms * 1000000ull;
}

static void pg_due(uint64_t t) {
  if (t < pg.scan_at) {
    pg.scan_at = t;
  }
}

// --sync-max: this turn has emitted every Synced event it may.
static bool pg_spent(void) {
  return host_conf.sync_max != 0 && pg.turn_synced >= host_conf.sync_max;
}

// ---- batches ----

static PgBatch* pg_batch(uint32_t id, uint8_t kind) {
  PgBatch* b = pg.spare;
  if (b != NULL) {
    pg.spare = b->link;
  } else {
    b = calloc(1, sizeof *b);
    if (b == NULL) {
      abort();
    }
  }
  b->id      = id;
  b->kind    = kind;
  b->n       = 0;
  b->failed  = false;
  b->refusal = false;
  b->link    = NULL;
  return b;
}

static void pg_batch_free(PgBatch* b) {
  for (uint32_t i = 0; i < b->n; i += 1) {
    free(b->st[i].text);
  }
  b->n     = 0;
  b->link  = pg.spare;
  pg.spare = b;
}

static void pg_event1(Env e, uint32_t tag, uint32_t b) {
  host_ev_begin();
  host_ev_word(tag);
  host_ev_word(b);
  host_ev_end(e);
}

static void pg_flushed(Env e, uint32_t b) {
  pg_event1(e, 1, b);
  pg.flushed += 1;
}

static void pg_died(Env e, PgBatch* b) {
  pg_event1(e, 3, b->id);
  pg.died += 1;
  pg_batch_free(b);
}

// ---- created_at ----

// Two decimal digits of v, 0 to 99, at p: a pair of pg_dd, "00" to "99".
static const char pg_dd[201] =
  "00010203040506070809" "10111213141516171819" "20212223242526272829" "30313233343536373839"
  "40414243444546474849" "50515253545556575859" "60616263646566676869" "70717273747576777879"
  "80818283848586878889" "90919293949596979899";

static char* pg_d2(char* p, uint32_t v) {
  memcpy(p, pg_dd + 2 * v, 2);
  return p + 2;
}

// The year as printf's "%04lld" writes it: at least four characters,
// zero-padded after the sign of a year before 0 (-1 is "-001").
static char* pg_year(char* p, int64_t y) {
  if (y >= 0 && y <= 9999) {
    p = pg_d2(p, (uint32_t)y / 100);
    return pg_d2(p, (uint32_t)y % 100);
  }
  uint64_t u = y < 0 ? 0 - (uint64_t)y : (uint64_t)y;
  char     t[20];
  int      k = 0;
  do {
    t[k++] = (char)('0' + u % 10);
    u /= 10;
  } while (u != 0);
  if (y < 0) {
    *p++ = '-';
  }
  for (int pad = (y < 0 ? 3 : 4) - k; pad > 0; pad -= 1) {
    *p++ = '0';
  }
  while (k > 0) {
    *p++ = t[--k];
  }
  return p;
}

// The date of the last day pg_iso wrote, "YYYY-MM-DDT" (11 bytes for the years 0 to 9999, 27 at
// the most). A day's date is the same text every time and a feed's posts are mostly of one
// day, so the next call for that day copies it. n 0: none kept. pg_outcome sets n to 0 before
// each statement's rows, so a date that came from a row is kept from one row of a reply to the
// next and never from one request to another (README.md:63-65 of the challenge: no state
// derived from a row outlives its request). pg_canned_sync (--db canned, no database) calls
// pg_iso with the clock's time for the post it makes up and does not set n to 0: there the date
// kept is the clock's day, read from no row, and it does pass from one request to the next.
static struct {
  int64_t  days;
  uint32_t n;
  char     text[28];
} pg_day;

// ms since the Unix epoch as SPEC.md:35 writes it: UTC, millisecond
// precision, Z (days to civil date: H. Hinnant's days_from_civil inverse),
// YYYY-MM-DDTHH:MM:SS.mmmZ, 24 bytes for the years 0 to 9999.
static void pg_iso(int64_t ms, JBuf* out) {
  int64_t days = ms / 86400000, rem = ms % 86400000;
  if (rem < 0) {
    rem += 86400000;
    days -= 1;
  }
  if (pg_day.n == 0 || pg_day.days != days) {
    int64_t  z   = days + 719468;
    int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
    uint64_t doe = (uint64_t)(z - era * 146097);
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t  y   = (int64_t)yoe + era * 400;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint64_t mp  = (5 * doy + 2) / 153;
    uint64_t d   = doy - (153 * mp + 2) / 5 + 1;
    uint64_t m   = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    char* q = pg_year(pg_day.text, y);  // a 20-character year at the most
    *q++ = '-';
    q    = pg_d2(q, (uint32_t)m);
    *q++ = '-';
    q    = pg_d2(q, (uint32_t)d);
    *q++ = 'T';
    pg_day.days = days;
    pg_day.n    = (uint32_t)(q - pg_day.text);
  }
  // ms of the day, 0 to 86399999, as seconds and ms, then minutes, then hours
  uint32_t r = (uint32_t)rem, s = r / 1000, f = r - s * 1000, mi = s / 60, h = mi / 60;
  out->n = 0;
  jb_need(out, 48);  // the date's 27 bytes at the most (28 are copied) and 13 more
  memcpy(out->p, pg_day.text, sizeof pg_day.text);
  char* p = out->p + pg_day.n;
  p    = pg_d2(p, h);
  *p++ = ':';
  p    = pg_d2(p, mi - h * 60);
  *p++ = ':';
  p    = pg_d2(p, s - mi * 60);
  *p++ = '.';
  *p++ = (char)('0' + f / 100);
  p    = pg_d2(p, f % 100);
  *p++ = 'Z';
  out->n = (size_t)(p - out->p);
}

// A binary timestamptz: µs since 2000-01-01T00:00:00Z, truncated to ms.
static void pg_iso_pg(int64_t us, JBuf* out) {
  int64_t ms = us / 1000;
  if (us % 1000 < 0) {
    ms -= 1;
  }
  pg_iso(ms + 946684800000ll, out);
}

// ---- results to outcomes ----

// The columns each statement's rows carry (host/sql.h): SelectOne 1, FeedQuery and PostQuery
// the post object's 5, InsertPost id and created_at, InsertLike none (it returns no rows).
static const uint16_t pg_ncols[PG_KINDS] = { 1, 5, 5, 2, 0 };

static void pg_put64(char* p, uint64_t v) {
  for (int i = 7; i >= 0; i -= 1) {
    p[i] = (char)(v & 0xFF);
    v >>= 8;
  }
}

// Every column arrives binary: pgw_bind_exec asks for binary results, and with no Describe
// no RowDescription comes to say so again. An int8 or a timestamptz is 8 big-endian bytes.
static bool pg_int8(const PgwCol* c, int64_t* v) {
  if (c->n != 8) {
    return false;
  }
  *v = (int64_t)pgw_be64(c->p);
  return true;
}

static bool pg_id(const PgwCol* c, uint32_t* id) {
  int64_t v;
  if (!pg_int8(c, &v) || v < 1 || v > 0xFFFFFFFFll) {
    return false;
  }
  *id = (uint32_t)v;
  return true;
}

// A post-object row: id int8, body text, created_at timestamptz, username
// text, like_count int4 (SPEC.md:55-57), none NULL.
static bool pg_row_ok(const PgwCol* c) {
  uint32_t id;
  int64_t  ts;
  return pg_id(&c[0], &id) && c[1].n >= 0 && pg_int8(&c[2], &ts) && c[3].n >= 0 && c[4].n == 4
    && (int32_t)pgw_be32(c[4].p) >= 0;
}

// The statement's next row at or after p, split into c; where the message after it starts.
// pg_frame has framed every message from the statement's BindComplete to its CommandComplete,
// so p is a message start and only a NoticeResponse or a ParameterStatus lies between rows.
static const char* pg_row_next(const char* p, PgwCol* c, uint16_t n) {
  while (p[0] != 'D') {
    p += 1 + pgw_be32(p + 1);
  }
  uint32_t len = pgw_be32(p + 1);
  pgw_cols(p + 5, len - 4, c, n);
  return p + 1 + len;
}

// A row held behind a handle, which is the answer: body and author stay in the receive
// buffer, which pg_synced then pins until pg_release; created_at is formatted now. *id and
// *likes are the row's.
static uint32_t pg_row_hold(const PgwCol* c, uint32_t req, uint32_t* id, uint32_t* likes) {
  int64_t ts;
  pg_id(&c[0], id);
  pg_int8(&c[2], &ts);
  pg_iso_pg(ts, &pg_ts);
  *likes = pgw_be32(c[4].p);
  return pg_hold(req, *id, *likes, c[1].p, (size_t)c[1].n, pg_ts.p, pg_ts.n, c[3].p, (size_t)c[3].n);
}

// A row as one Row{id, likes, h} node.
static void pg_row_put(const PgwCol* c, uint32_t req) {
  uint32_t id, likes;
  uint32_t h = pg_row_hold(c, req, &id, &likes);
  host_ev_row(id, likes, h);
}

// A write refused by an integrity constraint (SQLSTATE class 23, SPEC.md:61):
// not a failure of the batch; the statement is answered without being run
// again (pg_resend).
static bool pg_refused(const PgBatch* b, const PgStmt* s, const char* code) {
  return b->kind == K_WRITE && (s->kind == 3 || s->kind == 4) && code[0] == '2' && code[1] == '3';
}

// The statement at or after i that the batch still sends: refused ones go no more.
static uint32_t pg_skip(const PgBatch* b, uint32_t i) {
  while (i < b->n && b->st[i].refused) {
    i += 1;
  }
  return i;
}

// "INSERT oid rows", the CommandComplete of an InsertLike, read as libpq's PQcmdTuples reads
// it: the rows, 0 or 1; -1 for anything else.
static int8_t pg_inserted(const PgwMsg* m) {
  const char* z = memchr(m->body, 0, m->len);
  if (z == NULL || z - m->body < 7 || memcmp(m->body, "INSERT ", 7) != 0) {
    return -1;
  }
  const char* p = memchr(m->body + 7, ' ', (size_t)(z - m->body - 7));
  return p != NULL && z - p == 2 && (p[1] == '0' || p[1] == '1') ? (int8_t)(p[1] - '0') : -1;
}

// One statement's outcome, after its req: [kind, fields] (host/wire.bend).
// True when a row crossed as a handle: the receive buffer must be pinned.
static bool pg_outcome(const PgStmt* s) {
  int    nt = s->ok ? (int)s->nrows : -1;
  PgwCol c[5];
  pg_day.n = 0;  // pg_iso's date is kept within this statement's rows only
  switch (s->kind) {
  case 0:
    host_ev_word(nt == 1 ? OUT_PING : OUT_FAILED);
    return false;
  case 1: {
    // A feed of over 20 rows, or one with a row that is no post object, is one OUT_FAILED and no
    // row: every row is checked before the first is put. Each row is split once, into cs.
    if (nt < 0 || nt > 20) {
      host_ev_word(OUT_FAILED);
      return false;
    }
    PgwCol      cs[20][5];
    const char* p = s->at;
    for (int i = 0; i < nt; i += 1) {
      p = pg_row_next(p, cs[i], 5);
      if (!pg_row_ok(cs[i])) {
        host_ev_word(OUT_FAILED);
        return false;
      }
    }
    host_ev_word(OUT_POSTS);
    host_ev_open();
    if (host_conf.rows_block) {
      // --rows block (S33): the rows are held as neighbours, handles first .. first + nt - 1,
      // and the answer crosses as one cell, Row{nt, 0, first}; none when there is no row.
      uint32_t first = 0, id, likes;
      if (nt > 0) {
        pg_apart(s->req);
      }
      for (int i = 0; i < nt; i += 1) {
        uint32_t h = pg_row_hold(cs[i], s->req, &id, &likes);
        if (i == 0) {
          first = h;
        }
      }
      if (nt > 0) {
        host_ev_row((uint32_t)nt, 0, first);
      }
    } else {
      for (int i = 0; i < nt; i += 1) {
        pg_row_put(cs[i], s->req);
      }
    }
    host_ev_close();
    return nt > 0;
  }
  case 2:
    if (nt == 0) {
      host_ev_word(OUT_NOT_FOUND);
    } else if (nt == 1 && (pg_row_next(s->at, c, 5), pg_row_ok(c))) {
      host_ev_word(OUT_POST);
      pg_row_put(c, s->req);
      return true;
    } else {
      host_ev_word(OUT_FAILED);
    }
    return false;
  case 3: {
    uint32_t id;
    int64_t  ts;
    if (s->refused) {
      host_ev_word(OUT_UNKNOWN);
    } else if (nt == 1 && (pg_row_next(s->at, c, 2), pg_id(&c[0], &id) && pg_int8(&c[1], &ts))) {
      pg_iso_pg(ts, &pg_ts);
      host_ev_word(OUT_COMMITTED);
      host_ev_word(id);
      host_ev_str(pg_ts.p, pg_ts.n);
    } else {
      host_ev_word(OUT_FAILED);
    }
    return false;
  }
  case 4:
    if (s->refused) {
      host_ev_word(OUT_NOT_FOUND);
    } else if (s->ok && s->tag >= 0) {
      host_ev_word(OUT_LIKED);
      host_ev_word(s->tag == 1 ? 0 : 1);
    } else {
      host_ev_word(OUT_FAILED);
    }
    return false;
  default:
    host_ev_word(OUT_FAILED);
    return false;
  }
}

// Synced{b, ok, uptime_s, outs}; outs is empty when ok is false (the kernel
// answers every request of a failed batch Unknown whatever outs holds).
// outs runs newest statement first, the order of the batch's entries in the
// kernel, so that answer (src/policy.bend) finds each entry's pair at the
// head of what is left: O(statements) per batch. With unique request ids
// (A6) the replies do not depend on the order, only their cost does: this
// order is an expectation, not an obligation. When a row crossed, the slot's
// receive buffer is pinned until pg_release.
static void pg_synced(Env e, PgSlot* sl, PgBatch* b, bool ok, uint64_t now) {
  uint32_t first = held.base + held.n;
  bool     rows  = false;
  host_ev_begin();
  host_ev_word(2);
  host_ev_word(b->id);
  host_ev_word(ok ? 1 : 0);
  host_ev_word(host_uptime(now));
  host_ev_open();
  for (uint32_t j = b->n; ok && j > 0; j -= 1) {
    PgStmt* s = &b->st[j - 1];
    host_ev_open();
    host_ev_word(s->req);
    rows |= pg_outcome(s);
    host_ev_close();
  }
  host_ev_close();
  host_ev_rows(first, held.base + held.n - first);
  host_ev_end(e);
  if (rows) {
    pgw_pin(&sl->w);
  }
  pg.synced      += 1;
  pg.turn_synced += 1;
  pg.failed += ok ? 0 : 1;
  pg_batch_free(b);
}

// ---- --db canned ----

// A canned row held behind a handle, which is the answer.
static uint32_t pg_canned_hold(const PgRow* w, uint32_t req) {
  return pg_hold(req, w->id, w->likes, w->body, w->bn, w->ts, w->tn, w->author, w->an);
}

static void pg_canned_row(const PgRow* w, uint32_t req) {
  host_ev_row(w->id, w->likes, pg_canned_hold(w, req));
}

// outs newest first, as pg_synced.
static void pg_canned_sync(Env e, PgBatch* b, uint64_t now) {
  uint32_t first = held.base + held.n;
  host_ev_begin();
  host_ev_word(2);
  host_ev_word(b->id);
  host_ev_word(1);
  host_ev_word(host_uptime(now));
  host_ev_open();
  for (uint32_t j = b->n; j > 0; j -= 1) {
    PgStmt* s = &b->st[j - 1];
    host_ev_open();
    host_ev_word(s->req);
    switch (s->kind) {
    case 0:
      host_ev_word(OUT_PING);
      break;
    case 1:
      host_ev_word(OUT_POSTS);
      host_ev_open();
      if (host_conf.rows_block) {  // as pg_outcome: one cell for the answer
        uint32_t first = 0;
        if (pg.nfeed > 0) {
          pg_apart(s->req);
        }
        for (uint32_t k = 0; k < pg.nfeed; k += 1) {
          uint32_t h = pg_canned_hold(&pg.feed[k], s->req);
          if (k == 0) {
            first = h;
          }
        }
        if (pg.nfeed > 0) {
          host_ev_row(pg.nfeed, 0, first);
        }
      } else {
        for (uint32_t k = 0; k < pg.nfeed; k += 1) {
          pg_canned_row(&pg.feed[k], s->req);
        }
      }
      host_ev_close();
      break;
    case 2:
      host_ev_word(OUT_POST);
      pg_canned_row(&pg.post[s->pid == 250000 ? 1 : s->pid == 500000 ? 2 : 0], s->req);
      break;
    case 3: {
      struct timespec t;
      clock_gettime(CLOCK_REALTIME, &t);
      pg_iso((int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000, &pg_ts);
      host_ev_word(OUT_COMMITTED);
      host_ev_word(pg.next_post++);
      host_ev_str(pg_ts.p, pg_ts.n);
      break;
    }
    default:
      host_ev_word(OUT_LIKED);
      host_ev_word(0);
    }
    host_ev_close();
  }
  host_ev_close();
  host_ev_rows(first, held.base + held.n - first);
  host_ev_end(e);
  pg.synced      += 1;
  pg.turn_synced += 1;
  pg_batch_free(b);
}

static JDoc pg_doc;
static JBuf pg_file;

static char* pg_dup(const char* p, size_t n) {
  char* s = malloc(n + 1);
  if (s == NULL) {
    abort();
  }
  memcpy(s, p, n);
  s[n] = 0;
  return s;
}

static bool pg_golden_row(int32_t o, PgRow* w) {
  const JDoc* d = &pg_doc;
  int32_t id = json_get(d, o, "id"), body = json_get(d, o, "body");
  int32_t ts = json_get(d, o, "created_at"), au = json_get(d, o, "author");
  int32_t lc = json_get(d, o, "like_count");
  if (id < 0 || body < 0 || ts < 0 || au < 0 || lc < 0 || d->v[id].kind != J_NUM
    || d->v[body].kind != J_STR || d->v[ts].kind != J_STR || d->v[au].kind != J_STR
    || d->v[lc].kind != J_NUM) {
    return false;
  }
  w->id     = (uint32_t)d->v[id].num;
  w->likes  = (uint32_t)d->v[lc].num;
  w->body   = pg_dup(json_text(d, body), d->v[body].n);
  w->ts     = pg_dup(json_text(d, ts), d->v[ts].n);
  w->author = pg_dup(json_text(d, au), d->v[au].n);
  w->bn     = (uint32_t)strlen(w->body);
  w->tn     = (uint32_t)strlen(w->ts);
  w->an     = (uint32_t)strlen(w->author);
  return w->tn <= sizeof ((HostRow*)0)->ts;
}

static int32_t pg_golden(const char* name) {
  char path[4096];
  snprintf(path, sizeof path, "%s/%s", host_conf.golden, name);
  FILE* f = fopen(path, "rb");
  if (f == NULL) {
    return -1;
  }
  pg_file.n = 0;
  char   t[8192];
  size_t k;
  while ((k = fread(t, 1, sizeof t, f)) > 0) {
    jb_put(&pg_file, t, k);
  }
  fclose(f);
  return json_parse(&pg_doc, pg_file.p, pg_file.n);
}

static const char* pg_canned_load(void) {
  int32_t r = pg_golden("feed.json");
  int32_t a = json_get(&pg_doc, r, "posts");
  if (a < 0 || pg_doc.v[a].kind != J_ARR || pg_doc.v[a].n > 20) {
    return "--golden: feed.json is not {\"posts\":[..]} with at most 20 posts";
  }
  for (int32_t c = pg_doc.v[a].child; c >= 0; c = pg_doc.v[c].next) {
    if (!pg_golden_row(c, &pg.feed[pg.nfeed++])) {
      return "--golden: a post in feed.json lacks a field or has a created_at over 40 bytes";
    }
  }
  const char* names[3] = { "post-1.json", "post-250000.json", "post-500000.json" };
  for (int i = 0; i < 3; i += 1) {
    r = pg_golden(names[i]);
    if (!pg_golden_row(json_get(&pg_doc, r, "post"), &pg.post[i])) {
      return "--golden: post-1.json, post-250000.json or post-500000.json is not {\"post\":{..}}";
    }
  }
  pg.next_post = 500001;
  return NULL;
}

// ---- connections ----

static uint64_t pg_data(const PgSlot* s) {
  return HOST_TAG_PG | (uint64_t)(s - pg.slot);
}

static int pg_watch(PgSlot* s, uint32_t events) {
  return host_rewatch(s->w.fd, events, pg_data(s));
}

static int pg_cancel_watch(PgSlot* s) {
  return host_rewatch(s->cx.fd, pgw_cancel_events(&s->cx), pg_data(s) | PG_CANCEL);
}

// The database class's segments (S23): every socket this process has closed so far, each added
// just before it is closed (pg_down, pg_cancel_drop). The sockets are host/pgwire.c's and only
// those two close them, so each socket's whole life counts, the cancel connections' too.
// pg_close (Epoll.close) adds nothing: no line reports the class after it.
static HostSegs pg_gone;

// Closes a cancel connection, its counts added to pg_gone first.
static void pg_cancel_drop(PgSlot* s) {
  if (s->cx.fd >= 0) {
    host_unwatch(s->cx.fd);
    host_segs_add(&pg_gone, s->cx.fd);
  }
  pgw_cancel_close(&s->cx);
}

// A cancel connection in flight from now, for at most --db-timeout (pg_scan).
static bool pg_cancel_start(PgSlot* s, uint64_t now) {
  if (pgw_cancel_open(&s->cx, &s->w) == 0) {
    if (host_watch(s->cx.fd, pgw_cancel_events(&s->cx), pg_data(s) | PG_CANCEL) == 0) {
      pg.cancels += 1;
      s->csince   = now;
      pg_due(now + pg_ms(host_conf.db_timeout_ms));
      return true;
    }
    snprintf(s->cx.err, sizeof s->cx.err, "epoll_ctl: %s", strerror(errno));
  }
  fprintf(stderr, "bend-epoll: db slot %u: no cancel sent: %s\n", (unsigned)(s - pg.slot), s->cx.err);
  pg_cancel_drop(s);
  return false;
}

// The cancel in flight has ended: the first waiting slot that still has its
// connection sends its own.
static void pg_cancel_end(PgSlot* s, uint64_t now) {
  pg_cancel_drop(s);
  pg.cancels -= 1;
  for (uint32_t i = 0; i < pg.nslot && pg.cancels == 0; i += 1) {
    PgSlot* t = &pg.slot[i];
    if (t->cwait) {
      t->cwait = false;
      if (t->state == SLOT_CLOSING && t->w.fd >= 0) {
        pg_cancel_start(t, now);
      }
    }
  }
}

// Starts a cancel of what the backend runs, non-blocking (pgw_cancel_open, then
// pgw_cancel_poll from pg_ready); while another cancel is in flight the slot
// waits for it (pg_cancel_end), so at most one cancel connection is open. A
// connect not through its startup has no backend key, and nothing to cancel.
static void pg_cancel(PgSlot* s, uint64_t now) {
  if (!s->w.keyed) {
    return;
  }
  if (pg.cancels > 0) {
    s->cwait = true;
    pg.cancel_waits += 1;
    if (host_trace != NULL) {
      host_tl_begin();
      jb_str(&host_tl, "\"note\":\"cancel_wait\",\"slot\":");
      jb_u64(&host_tl, (uint64_t)(s - pg.slot));
      jb_str(&host_tl, ",\"waits\":");
      jb_u64(&host_tl, pg.cancel_waits);
      host_tl_end();
    }
    return;
  }
  pg_cancel_start(s, now);
}

static void pg_cancel_poll(PgSlot* s, uint32_t events, uint64_t now) {
  switch (pgw_cancel_poll(&s->cx, events)) {
  case PGW_WAIT:
    if (pg_cancel_watch(s) == 0) {
      return;
    }
    break;
  case PGW_DONE:
    break;
  default:
    fprintf(stderr, "bend-epoll: db slot %u: cancel failed: %s\n", (unsigned)(s - pg.slot), s->cx.err);
  }
  pg_cancel_end(s, now);
}

// Closes what the slot holds and schedules its reconnect.
static void pg_down(PgSlot* s, uint64_t now) {
  s->cwait = false;
  if (s->cx.fd >= 0) {
    pg_cancel_end(s, now);
  }
  if (s->w.fd >= 0) {
    host_unwatch(s->w.fd);
    host_segs_add(&pg_gone, s->w.fd);
  }
  pgw_close(&s->w);
  s->state   = SLOT_DOWN;
  s->out     = false;
  s->retry   = now + pg_ms(s->backoff);
  s->backoff = s->backoff * 2 > PG_BACKOFF_MAX_MS ? PG_BACKOFF_MAX_MS : s->backoff * 2;
  pg_due(s->retry);
}

// Every batch the slot carries dies. A socket still open is shut for writing,
// its backend gets a cancel, and the slot waits in SLOT_CLOSING for the server
// to close it (pg_drain); no deadline, so --pool bounds the backends. A socket
// that read an end of file or an error (w.bad) is closed at once.
// A slot that --sync-max left pending dies with the answers it held in its
// receive buffer: they are Died, as the answers read in the same recv as a failure are.
static void pg_kill(Env e, PgSlot* s, uint64_t now, const char* why) {
  bool held = s->pending;
  if (held) {
    s->pending  = false;
    pg.pending -= 1;
  }
  uint32_t lost = s->inflight;
  for (PgBatch* b = s->head; b != NULL;) {
    PgBatch* next = b->link;
    pg_died(e, b);
    b = next;
  }
  s->head     = NULL;
  s->tail     = NULL;
  s->inflight = 0;
  s->writes   = 0;
  const char* m = s->w.err;
  fprintf(stderr, "bend-epoll: db slot %u down (%s), %u batches died%s: %.*s\n",
    (unsigned)(s - pg.slot), why, lost, held ? " (it held answers for the next round)" : "",
    (int)strcspn(m, "\n"), m);
  if (s->w.fd >= 0 && !s->w.bad && shutdown(s->w.fd, SHUT_WR) == 0) {
    s->state = SLOT_CLOSING;
    s->since = now;
    s->out   = false;
    if (pg_watch(s, EPOLLIN) == 0) {
      pg_cancel(s, now);
      return;
    }
  }
  pg_down(s, now);
}

// SLOT_CLOSING: what the backend still sends is dropped; at EOF (the backend
// has exited, its pg_stat_activity row gone before its socket closes) or a
// read error the slot goes down.
static void pg_drain(PgSlot* s, uint64_t now) {
  char t[4096];
  for (;;) {
    ssize_t k = recv(s->w.fd, t, sizeof t, MSG_DONTWAIT);
    if (k > 0 || (k < 0 && errno == EINTR)) {
      continue;
    }
    if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    break;
  }
  fprintf(stderr, "bend-epoll: db slot %u: the server closed the old connection after %llu ms\n",
    (unsigned)(s - pg.slot), (unsigned long long)((now - s->since) / 1000000ull));
  pg_down(s, now);
}

static void pg_connect(Env e, PgSlot* s, uint64_t now) {
  s->state = SLOT_CONNECTING;
  s->since = now;
  pg_due(now + pg_ms(host_conf.db_timeout_ms));
  if (pgw_open(&s->w) < 0 || host_watch(s->w.fd, pgw_events(&s->w), pg_data(s)) < 0) {
    pg_kill(e, s, now, "connect");
  }
}

// The five statements, each with a Parse, then one Sync; ParseComplete for each and a
// ReadyForQuery make the slot ready (pg_results).
static void pg_prepare(Env e, PgSlot* s, uint64_t now) {
  static const uint32_t one[1] = { PG_OID_INT8 };
  static const uint32_t ins[2] = { PG_OID_INT8, PG_OID_TEXT };
  static const uint32_t two[2] = { PG_OID_INT8, PG_OID_INT8 };
  const uint32_t*       types[PG_KINDS] = { NULL, NULL, one, ins, two };
  for (int k = 0; k < PG_KINDS; k += 1) {
    pgw_parse(&s->w, pg_sql[k].name, pg_sql[k].text, (uint16_t)pg_sql[k].nparams, types[k]);
  }
  pgw_sync(&s->w);
  int fl = pgw_flush(&s->w);
  if (fl < 0) {
    pg_kill(e, s, now, "prepare");
    return;
  }
  s->state  = SLOT_PREPARING;
  s->since  = now;
  s->next   = 0;
  s->failed = false;
  s->out    = fl == 1;
  if (pg_watch(s, EPOLLIN | (s->out ? EPOLLOUT : 0)) < 0) {
    pg_kill(e, s, now, "watch");
  }
}

// SLOT_CONNECTING: the connect, the startup and its authentication (pgw_startup).
static void pg_connect_poll(Env e, PgSlot* s, uint32_t events, uint64_t now) {
  switch (pgw_startup(&s->w, events)) {
  case PGW_WAIT:
    if (pg_watch(s, pgw_events(&s->w)) < 0) {
      pg_kill(e, s, now, "watch");
    }
    return;
  case PGW_DONE:
    pg_prepare(e, s, now);
    return;
  default:
    pg_kill(e, s, now, "connect");
  }
}

static void pg_resend(Env e, PgSlot* s, PgBatch* b, uint64_t now);

// The replies to b's attempt, from the slot's receive buffer through the ReadyForQuery of its
// Sync, which has arrived (pg_results). Per statement sent: BindComplete, a DataRow per row,
// CommandComplete. An ErrorResponse ends the statement it falls in, before or after its
// BindComplete, and PostgreSQL skips the rest of the attempt to the Sync, so nothing more comes
// for it (libpq's PGRES_PIPELINE_ABORTED): a refusal (pg_refused) marks the statement refused
// and the attempt b->refusal; any other error, an error after the last statement (one at
// commit) and an EmptyQueryResponse fail b. NoticeResponse and ParameterStatus may come
// anywhere. Each statement's rows stay where they are (at, nrows) for pg_outcome. The
// ReadyForQuery must say idle. NULL, or why the replies cannot be framed: then the connection
// fails closed (pg_kill), its batches Died.
static const char* pg_frame(PgSlot* s, PgBatch* b) {
  uint32_t i       = pg_skip(b, 0);  // the statement whose replies come next
  PgStmt*  t       = NULL;           // the statement between its BindComplete and its end
  bool     aborted = false;          // an ErrorResponse came: only ReadyForQuery is left
  PgwMsg   m;
  PgwCol   c[5];
  while (pgw_take(&s->w, &m)) {
    switch (m.type) {
    case 'N':
    case 'S':
      break;
    case '2':
      if (t != NULL || aborted || i >= b->n) {
        return "a BindComplete out of place";
      }
      t        = &b->st[i];
      t->at    = m.body + m.len;
      t->nrows = 0;
      break;
    case 'D':
      if (t == NULL) {
        return "a DataRow outside an Execute";
      }
      if (pg_ncols[t->kind] == 0 || !pgw_cols(m.body, m.len, c, pg_ncols[t->kind])) {
        return "a DataRow whose columns do not match its statement's";
      }
      t->nrows += 1;
      break;
    case 'C':
      if (t == NULL) {
        return "a CommandComplete outside an Execute (a second one for one Execute?)";
      }
      t->ok = true;
      if (t->kind == 4) {
        t->tag = pg_inserted(&m);
      }
      t = NULL;
      i = pg_skip(b, i + 1);
      break;
    case 'I':
      if (t == NULL) {
        return "an EmptyQueryResponse outside an Execute";
      }
      b->failed = true;
      t         = NULL;
      i         = pg_skip(b, i + 1);
      break;
    case 'E': {
      if (aborted) {
        return "a second ErrorResponse before the Sync";
      }
      char code[6];
      pgw_error(&s->w, &m, code);
      if (i < b->n && !b->refusal && pg_refused(b, &b->st[i], code)) {
        b->st[i].refused = true;
        b->refusal       = true;
        pg.refused      += 1;
      } else {
        b->failed = true;
      }
      aborted = true;
      t       = NULL;
      break;
    }
    case 'Z':
      if (!aborted && (t != NULL || i < b->n)) {
        return "a ReadyForQuery before every statement answered";
      }
      if (m.len != 1) {
        return "a ReadyForQuery of the wrong length";
      }
      // The status after our Sync: idle, the implicit transaction ended (committed, or rolled
      // back after an ErrorResponse, which reads idle too). In a transaction ('T') or a failed
      // one ('E'), the server did not end it, so nothing of the batch committed.
      return m.body[0] == 'I' ? NULL : "a ReadyForQuery whose status is not idle after the Sync";
    default:
      return "a message out of place among a batch's replies";
    }
  }
  return "a batch's replies cut short";  // not reached: pg_results waits for the ReadyForQuery
}

// The replies in the slot's receive buffer. A prepare's are ParseComplete per statement, then
// ReadyForQuery; an ErrorResponse among them fails the prepare. A ready slot with batches in
// flight frames the oldest once its ReadyForQuery has arrived (pg_frame) and ends it: Synced,
// or a resend after a refusal (pg_resend). With none in flight only NoticeResponse and
// ParameterStatus may come; anything else (the ErrorResponse a terminated backend sends before
// it closes, a ReadyForQuery no Sync asked for) fails the connection.
static void pg_results(Env e, PgSlot* s, uint64_t now) {
  PgwMsg m;
  char   code[6];
  while (s->state == SLOT_PREPARING && pgw_take(&s->w, &m)) {
    if (m.type == '1') {
      s->next += 1;
    } else if (m.type == 'E') {
      pgw_error(&s->w, &m, code);
      s->failed = true;
    } else if (m.type == 'Z') {
      if (s->failed || s->next != PG_KINDS || m.len != 1 || m.body[0] != 'I') {
        pg_kill(e, s, now, "prepare failed");
        return;
      }
      s->state   = SLOT_READY;
      s->backoff = PG_BACKOFF_MIN_MS;
      pg.open[K_READ].stalled  = false;
      pg.open[K_WRITE].stalled = false;
    } else if (m.type != 'N' && m.type != 'S') {
      pg_kill(e, s, now, "prepare sequence");
      return;
    }
  }
  while (s->state == SLOT_READY) {
    PgBatch* b = s->head;
    if (b == NULL) {
      if (!pgw_take(&s->w, &m)) {
        return;
      }
      if (m.type != 'N' && m.type != 'S') {
        if (m.type == 'E') {
          pgw_error(&s->w, &m, code);
        }
        pg_kill(e, s, now, "a message with no batch in flight");
        return;
      }
    } else if (s->w.in.zs == 0) {
      return;  // the ReadyForQuery that ends the oldest batch's replies has not arrived
    } else if (pg_spent()) {  // --sync-max: the rest waits in the receive buffer for pg_resume
      if (!s->pending) {
        s->pending    = true;
        pg.pending   += 1;
        pg.stops     += 1;
      }
      return;
    } else {
      const char* why = pg_frame(s, b);
      if (why != NULL) {
        pg_kill(e, s, now, why);
        return;
      }
      s->head = b->link;
      if (s->head == NULL) {
        s->tail = NULL;
      }
      s->inflight -= 1;
      s->writes   -= b->kind == K_WRITE;
      if (b->refusal && !b->failed) {
        pg_resend(e, s, b, now);
      } else {
        pg_synced(e, s, b, !b->failed, now);
      }
      if (pg_spent()) {  // this Synced spent the turn's budget: next turn, pg_resume starts after s
        pg.rot = (uint32_t)(s - pg.slot);
      }
    }
  }
}

void pg_ready(Env e, uint64_t data, uint32_t events, uint64_t now) {
  uint32_t i = (uint32_t)(data & 0xFFFF);
  if (i >= pg.nslot) {
    return;
  }
  PgSlot* s = &pg.slot[i];
  if (s->state == SLOT_CLOSING) {
    if (!(data & PG_CANCEL)) {
      pg_drain(s, now);
    } else if (s->cx.fd >= 0) {
      pg_cancel_poll(s, events, now);
    }
    return;
  }
  if (s->state == SLOT_DOWN || (data & PG_CANCEL)) {
    return;
  }
  if (s->state == SLOT_CONNECTING) {
    pg_connect_poll(e, s, events, now);
    return;
  }
  if (events & EPOLLOUT) {
    int fl = pgw_flush(&s->w);
    if (fl < 0) {
      pg_kill(e, s, now, "write");
      return;
    }
    if (fl == 0 && s->out) {
      s->out = false;
      if (pg_watch(s, EPOLLIN) < 0) {
        pg_kill(e, s, now, "watch");
        return;
      }
    }
  }
  if (events & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
    if (pgw_recv(&s->w) < 0) {
      pg_kill(e, s, now, "read");
      return;
    }
  }
  pg_results(e, s, now);
}

// ---- the open batches ----

// The ready connection with the fewest batches in flight among the kind's
// slots: with --write-slots N, the last N slots for a write batch and the
// others for a read batch. With --write-slots 0, the pick before S15: any
// slot, and for a read batch one that carries no write batch if there is one.
// Ties go to the first in a scan that starts one slot further each pick; the
// start rotates over the kind's own slots, so each of them starts it as often
// (one counter for both kinds at --write-slots 0, as before S15).
static PgSlot* pg_pick(uint8_t kind) {
  uint32_t  ws = host_conf.write_slots, lo = 0, n = pg.nslot;
  bool      avoid = kind == K_READ && ws == 0;
  uint32_t* start = &pg.pick[K_READ];
  if (ws > 0) {  // the kind check: tests/host/pool_test.mjs builds a mutant without it
    lo    = kind == K_WRITE ? pg.nslot - ws : 0;
    n     = kind == K_WRITE ? ws : pg.nslot - ws;
    start = &pg.pick[kind];
  }
  PgSlot* best = NULL;
  for (uint32_t k = 0; k < n; k += 1) {
    PgSlot* s = &pg.slot[lo + (*start + k) % n];
    if (s->state != SLOT_READY) {
      continue;
    }
    bool behind = avoid && s->writes > 0;
    bool worse  = best != NULL && avoid && best->writes > 0;
    if (best == NULL || (worse && !behind) || (behind == worse && s->inflight < best->inflight)) {
      best = s;
      if (s->inflight == 0) {
        break;
      }
    }
  }
  *start = (*start + 1) % (n ? n : 1);
  return best;
}

// One attempt of b: Bind and Execute for every statement not refused, then one Sync, all
// written as one buffer.
static bool pg_send(PgSlot* s, PgBatch* b) {
  b->refusal = false;
  for (uint32_t i = pg_skip(b, 0); i < b->n; i = pg_skip(b, i + 1)) {
    PgStmt*     t = &b->st[i];
    char        p1[8], p2[8];
    const char* vals[2] = { p1, p2 };
    uint32_t    lens[2] = { 8, 8 };
    switch (t->kind) {
    case 2:
      pg_put64(p1, t->pid);
      break;
    case 3:
      pg_put64(p1, t->uid);
      vals[1] = t->text;
      lens[1] = t->tlen;
      break;
    case 4:
      pg_put64(p1, t->uid);
      pg_put64(p2, t->pid);
      break;
    }
    t->ok    = false;
    t->tag   = -1;
    t->at    = NULL;
    t->nrows = 0;
    const PgSql* q = &pg_sql[t->kind];
    pgw_bind_exec(&s->w, q->name, (uint16_t)q->nparams, vals, lens);
  }
  pgw_sync(&s->w);
  pg.syncs += 1;
  int fl = pgw_flush(&s->w);
  if (fl < 0) {
    return false;
  }
  if (fl == 1 && !s->out) {
    s->out = true;
    return pg_watch(s, EPOLLIN | EPOLLOUT) == 0;
  }
  return true;
}

// b joins the tail of the slot's FIFO, its attempt sent now.
static void pg_enqueue(PgSlot* s, PgBatch* b, uint64_t now) {
  b->sent = now;
  b->link = NULL;
  if (s->tail == NULL) {
    s->head = b;
  } else {
    s->tail->link = b;
  }
  s->tail      = b;
  s->inflight += 1;
  s->writes   += b->kind == K_WRITE;
  pg_due(now + pg_ms(host_conf.db_timeout_ms));
}

// b's attempt ended at its Sync with refusals and no other failure, so the
// server rolled it back: the statements not refused go again, as b's next
// attempt at the slot's FIFO tail. When every statement is refused, nothing
// is left to send and b is Synced (ok, each statement refused).
static void pg_resend(Env e, PgSlot* s, PgBatch* b, uint64_t now) {
  uint32_t left = 0;
  for (uint32_t i = 0; i < b->n; i += 1) {
    left += !b->st[i].refused;
  }
  if (host_trace != NULL) {
    host_tl_begin();
    jb_str(&host_tl, "\"note\":\"resend\",\"batch\":");
    jb_u64(&host_tl, b->id);
    jb_str(&host_tl, ",\"refused\":[");
    bool comma = false;
    for (uint32_t i = 0; i < b->n; i += 1) {
      if (b->st[i].refused) {
        jb_str(&host_tl, comma ? "," : "");
        jb_u64(&host_tl, b->st[i].req);
        comma = true;
      }
    }
    jb_str(&host_tl, "],\"left\":");
    jb_u64(&host_tl, left);
    host_tl_end();
  }
  if (left == 0) {
    pg_synced(e, s, b, true, now);
    return;
  }
  pg.resent += 1;
  pg_enqueue(s, b, now);
  if (!pg_send(s, b)) {
    pg_kill(e, s, now, "send");
  }
}

static void pg_close_open(PgOpen* o) {
  pg.stmts          += o->b->n;
  pg.flushed_writes += o->b->kind == K_WRITE;
  o->b       = NULL;
  o->ok      = false;
  o->stalled = false;
}

static void pg_flush(Env e, PgOpen* o, uint64_t now) {
  PgBatch* b = o->b;
  if (host_conf.db == DB_CANNED) {
    pg_close_open(o);
    pg_flushed(e, b->id);
    if (pg_spent()) {  // --sync-max: Synced at the next turn's start (pg_resume)
      b->link = NULL;
      if (pg.deferred_tail == NULL) {
        pg.deferred = b;
      } else {
        pg.deferred_tail->link = b;
      }
      pg.deferred_tail = b;
      pg.held         += 1;
    } else {
      pg_canned_sync(e, b, now);
    }
    return;
  }
  PgSlot* s = pg_pick(b->kind);
  if (s == NULL) {
    if (now >= o->first + pg_ms(host_conf.db_timeout_ms)) {
      pg_close_open(o);
      pg_flushed(e, b->id);
      pg_died(e, b);
    } else {
      o->stalled = true;
    }
    return;
  }
  pg_close_open(o);
  pg_enqueue(s, b, now);
  bool sent = pg_send(s, b);
  pg_flushed(e, b->id);
  if (!sent) {
    pg_kill(e, s, now, "send");
  }
}

// Deadlines of the slots: reconnects, connects and prepares that take too
// long, batches past --db-timeout.
static void pg_scan(Env e, uint64_t now) {
  uint64_t tmo = pg_ms(host_conf.db_timeout_ms);
  pg.scan_at   = UINT64_MAX;
  for (uint32_t i = 0; i < pg.nslot; i += 1) {
    PgSlot* s = &pg.slot[i];
    switch (s->state) {
    case SLOT_DOWN:
      if (now >= s->retry) {
        pg_connect(e, s, now);
      } else {
        pg_due(s->retry);
      }
      break;
    case SLOT_CONNECTING:
    case SLOT_PREPARING:
      if (now >= s->since + tmo) {
        pg_kill(e, s, now, "connect timeout");
      } else {
        pg_due(s->since + tmo);
      }
      break;
    case SLOT_CLOSING:
      // No deadline for the slot: back to SLOT_DOWN when the server closes the socket
      // (pg_drain). Its cancel connection, if in flight, has --db-timeout to connect, send and
      // see the server close.
      if (s->cx.fd >= 0 && now >= s->csince + tmo) {
        fprintf(stderr, "bend-epoll: db slot %u: cancel given up after --db-timeout\n", i);
        pg_cancel_end(s, now);
      } else if (s->cx.fd >= 0) {
        pg_due(s->csince + tmo);
      }
      break;
    case SLOT_READY:
      // A pending slot's answers wait in its receive buffer for this host (--sync-max), not for
      // the server: no timeout while it is pending. Its deadline stays due, so the scan comes
      // back once pg_resume has taken them, and a batch still unanswered then is timed out as before.
      if (s->head != NULL && !s->pending && now >= s->head->sent + tmo) {
        pg_kill(e, s, now, "batch timeout");
      } else if (s->head != NULL) {
        pg_due(s->head->sent + tmo);
      }
      break;
    }
  }
}

// The kind's clock in ns: --batch-wait for the read batch, --write-wait for the
// write batch.
static uint64_t pg_wait(uint32_t kind) {
  return (uint64_t)(kind == K_WRITE ? host_conf.write_wait_us : host_conf.batch_wait_us) * 1000ull;
}

// A batch's statement cap: --read-batch-max for the read batch, --batch-max for the write batch.
static uint32_t pg_cap(uint32_t kind) {
  return kind == K_READ ? host_conf.read_batch_max : host_conf.batch_max;
}

static bool pg_due_open(const PgOpen* o, uint32_t kind, uint64_t now) {
  return o->ok && o->b != NULL && o->b->n > 0 && (o->b->n >= pg_cap(kind) || now >= o->first + pg_wait(kind));
}

// The answers the last turn's budget left (--sync-max), before anything else this turn: no
// socket will report a pending slot again. The scan starts after the slot that spent the last
// budget, so every other pending slot goes before that one goes again; it stops when this
// turn's budget is spent, and the slots it did not reach stay pending. Canned batches deferred
// the same way are Synced oldest first.
static void pg_resume(Env e, uint64_t now) {
  while (pg.deferred != NULL && !pg_spent()) {
    PgBatch* b  = pg.deferred;
    pg.deferred = b->link;
    if (pg.deferred == NULL) {
      pg.deferred_tail = NULL;
    }
    pg_canned_sync(e, b, now);
  }
  uint32_t from = pg.rot;
  for (uint32_t k = 1; k <= pg.nslot && pg.pending > 0 && !pg_spent(); k += 1) {
    PgSlot* s = &pg.slot[(from + k) % pg.nslot];
    if (s->pending) {
      s->pending  = false;
      pg.pending -= 1;
      pg_results(e, s, now);
    }
  }
}

// The turn's start: a new --sync-max budget and the answers held over from the last turn
// (pg_resume); then each open batch that is full or has waited its kind's clock flushes, the
// read batch first; then the slots' deadlines.
void pg_tick(Env e, uint64_t now) {
  pg.turn_synced = 0;
  pg_resume(e, now);
  for (uint32_t k = K_READ; k <= K_WRITE; k += 1) {
    if (pg_due_open(&pg.open[k], k, now)) {
      pg_flush(e, &pg.open[k], now);
    }
  }
  if (now >= pg.scan_at) {
    pg_scan(e, now);
  }
}

// --window-us (host/epoll.c, ep_window): the open read batch flushes now, whatever its --batch-wait
// clock, so that the database answers it while the turn sleeps. The write batch keeps its clock.
void pg_flush_reads(Env e, uint64_t now) {
  PgOpen* o = &pg.open[K_READ];
  if (o->ok && o->b != NULL && o->b->n > 0) {
    pg_flush(e, o, now);
  }
}

bool pg_pending(void) {
  return pg.pending > 0 || pg.deferred != NULL;
}

uint64_t pg_next(uint64_t now) {
  uint64_t t = pg.scan_at;
  for (uint32_t k = K_READ; k <= K_WRITE; k += 1) {
    const PgOpen* o = &pg.open[k];
    if (o->ok && o->b != NULL && o->b->n > 0) {
      uint64_t due = o->stalled ? o->first + pg_ms(host_conf.db_timeout_ms) : o->first + pg_wait(k);
      t = due < t ? due : t;
    }
  }
  (void)now;
  return t;
}

// The requests a round may lex: the least room of the two open batches, each against its own
// cap (pg_cap), as each request may queue in either.
uint32_t pg_room(void) {
  uint32_t room = UINT32_MAX;
  for (uint32_t k = K_READ; k <= K_WRITE; k += 1) {
    const PgOpen* o   = &pg.open[k];
    uint32_t      n   = o->ok && o->b != NULL ? o->b->n : 0;  // between Flushed and Open the batch after is empty
    uint32_t      cap = pg_cap(k);
    uint32_t      r   = n >= cap ? 0 : cap - n;
    room              = r < room ? r : room;
  }
  return room;
}

void pg_close(void) {
  pg_release();
  for (uint32_t i = 0; i < pg.nslot; i += 1) {
    pgw_cancel_close(&pg.slot[i].cx);
    pgw_close(&pg.slot[i].w);
  }
}

void pg_report(void) {
  fprintf(stderr, "bend-epoll: db: %llu batches flushed (%llu reads, %llu writes; %llu statements, %.2f per batch),"
    " %llu synced (%llu of them failed), %llu died, %llu syncs, %llu statements refused (class 23), %llu resends,"
    " %llu cancels waited; --sync-max: %llu connection stops with answers left in the receive buffer, %llu canned batches held"
    " for the next turn\n", (unsigned long long)pg.flushed,
    (unsigned long long)(pg.flushed - pg.flushed_writes),
    (unsigned long long)pg.flushed_writes, (unsigned long long)pg.stmts,
    pg.flushed ? (double)pg.stmts / (double)pg.flushed : 0.0, (unsigned long long)pg.synced,
    (unsigned long long)pg.failed, (unsigned long long)pg.died, (unsigned long long)pg.syncs,
    (unsigned long long)pg.refused, (unsigned long long)pg.resent, (unsigned long long)pg.cancel_waits,
    (unsigned long long)pg.stops, (unsigned long long)pg.held);
}

// The stats file's database columns (host/epoll.c, --stats), once a second: the batches in flight
// by kind (a slot counts its write batches in writes), the one cancel in flight, and the slots
// whose cancel waits for it (cwait).
void pg_stats(PgStats* o) {
  memset(o, 0, sizeof *o);
  o->handles = held.n;
  o->cancels = pg.cancels;
  for (uint32_t i = 0; i < pg.nslot; i += 1) {
    const PgSlot* s = &pg.slot[i];
    o->busy_slots      += s->inflight > 0;
    o->write_batches   += s->writes;
    o->read_batches    += s->inflight - s->writes;
    o->cancels_waiting += s->cwait;
  }
}

// The database class's segments now: the closed sockets' sums and every open socket's
// counters, the pool's (connecting, ready or closing) and the cancel connection's.
void pg_segs(HostSegs* o) {
  *o = pg_gone;
  for (uint32_t i = 0; i < pg.nslot; i += 1) {
    const PgSlot* s = &pg.slot[i];
    if (s->w.fd >= 0) {
      host_segs_add(o, s->w.fd);
    }
    if (s->cx.fd >= 0) {
      host_segs_add(o, s->cx.fd);
    }
  }
}

// ---- Pg.open ----

Term pg_open_run(Env e, Term* f, IoWork* w) {
  if (host_db_set() < 0 || pg.started) {
    return io_fail(e, EBADF, "Pg.open runs once, after Epoll.open");
  }
  pg.started = true;
  if (host_conf.db == DB_CANNED) {
    const char* bad = pg_canned_load();
    if (bad != NULL) {
      return io_fail(e, ENOENT, bad);
    }
    io_sync();
    fprintf(stderr, "bend-epoll: --db canned, golden files from %s\n", host_conf.golden);
    return io_done(e, term_pak(CID(Unit), 0));
  }
  // The StartupMessage's parameters after user and database: they replace an application_name
  // or an options in DATABASE_URL.
  static const char* const params[] = { "application_name", "bend-epoll", "options",
    "-c client_connection_check_interval=1000", NULL };
  static char              why[256];
  const char*              url = getenv("DATABASE_URL");
  if (url == NULL) {
    return io_fail(e, EINVAL, "DATABASE_URL is required with --db pg");
  }
  if (pgw_target(url, params, why, sizeof why) < 0) {
    return io_fail(e, EINVAL, why);
  }
  pg.nslot = host_conf.pool;
  pg.slot  = calloc(pg.nslot, sizeof *pg.slot);
  if (pg.slot == NULL) {
    return io_fail(e, ENOMEM, NULL);
  }
  uint64_t now = io_tick();
  for (uint32_t i = 0; i < pg.nslot; i += 1) {
    pg.slot[i].w.fd    = -1;
    pg.slot[i].cx.fd   = -1;
    pg.slot[i].backoff = PG_BACKOFF_MIN_MS;
    pg_connect(e, &pg.slot[i], now);
  }
  uint64_t           until = now + pg_ms(host_conf.db_timeout_ms);
  struct epoll_event evs[64];
  uint32_t           ready = 0;
  while (now < until) {
    ready = 0;
    for (uint32_t i = 0; i < pg.nslot; i += 1) {
      ready += pg.slot[i].state == SLOT_READY;
    }
    if (ready == pg.nslot) {
      break;
    }
    int n = epoll_wait(host_db_set(), evs, 64, 10);
    now   = io_tick();
    for (int i = 0; i < n; i += 1) {
      pg_ready(e, evs[i].data.u64, evs[i].events, now);
    }
    if (now >= pg.scan_at) {
      pg_scan(e, now);
    }
  }
  io_sync();
  fprintf(stderr, "bend-epoll: --db pg, %u of %u connections ready\n", ready, pg.nslot);
  if (ready == 0) {
    return io_fail(e, ETIMEDOUT, "no database connection became ready within --db-timeout");
  }
  return io_done(e, term_pak(CID(Unit), 0));
}

// ---- the Queue and Open commands (host/epoll.c, ep_cmd) ----

static void pg_trace_queue(uint32_t b, const PgStmt* s) {
  host_tl_begin();
  jb_str(&host_tl, "\"c\":\"Queue\",\"batch\":");
  jb_u64(&host_tl, b);
  jb_str(&host_tl, ",\"req\":");
  jb_u64(&host_tl, s->req);
  jb_str(&host_tl, ",\"kind\":");
  jb_u64(&host_tl, s->kind);
  jb_str(&host_tl, ",\"uid\":");
  jb_u64(&host_tl, s->uid);
  jb_str(&host_tl, ",\"pid\":");
  jb_u64(&host_tl, s->pid);
  jb_str(&host_tl, ",\"name\":");
  json_quote(&host_tl, pg_name.p, pg_name.n);
  jb_str(&host_tl, ",\"text\":");
  json_quote(&host_tl, pg_text.p, pg_text.n);
  host_tl_end();
}

// f: batch, req, kind, uid, pid, name, text (host/epoll.bend, Queue).
void pg_queue(Env e, const Term* f) {
  uint32_t batch = (uint32_t)f[0];
  pg_name.n      = 0;
  pg_text.n      = 0;
  host_take_str(e, f[5], &pg_name, false);
  host_take_str(e, f[6], &pg_text, false);
  PgStmt   s    = { .req = (uint32_t)f[1], .kind = (uint8_t)f[2], .uid = (uint32_t)f[3], .pid = (uint32_t)f[4] };
  uint8_t  kind = (uint32_t)f[2] >= 3 ? K_WRITE : K_READ;
  PgOpen*  o    = &pg.open[kind];
  if (!o->ok || batch != o->id || (uint32_t)f[2] >= PG_KINDS) {
    fprintf(stderr, "bend-epoll: Queue{%u, %u, kind %u} while the %s batch %u is %s\n", batch, s.req,
      (unsigned)(uint32_t)f[2], kind == K_WRITE ? "write" : "read", o->id, o->ok ? "open" : "flushed");
    exit(70);
  }
  if (s.kind == 3) {
    s.text = pg_dup(pg_text.p, pg_text.n);
    s.tlen = (uint32_t)pg_text.n;
  }
  if (o->b == NULL) {
    o->b     = pg_batch(batch, kind);
    o->first = io_tick();
  }
  PgBatch* b = o->b;
  if (b->n == b->cap) {
    b->cap = b->cap ? b->cap * 2 : host_conf.batch_max;
    b->st  = realloc(b->st, b->cap * sizeof *b->st);
    if (b->st == NULL) {
      abort();
    }
  }
  b->st[b->n++] = s;
  if (host_trace != NULL) {
    pg_trace_queue(batch, &s);
  }
}

// Open{batch, kind}: the kind's last batch was flushed, and batch is the id
// after the last one opened (A6.8, per kind).
void pg_open_batch(uint32_t batch, uint32_t kind) {
  if (kind > K_WRITE || pg.open[kind].ok || batch != pg.expect) {
    fprintf(stderr, "bend-epoll: Open{%u, kind %u} while the next id is %u and the batch of that kind is %s\n",
      batch, kind, pg.expect, kind > K_WRITE ? "none" : pg.open[kind].ok ? "open" : "flushed");
    exit(70);
  }
  pg.open[kind].id = batch;
  pg.open[kind].ok = true;
  pg.expect        = batch + 1;
  if (host_trace != NULL) {
    host_tl_begin();
    jb_str(&host_tl, "\"c\":\"Open\",\"batch\":");
    jb_u64(&host_tl, batch);
    jb_str(&host_tl, kind == K_WRITE ? ",\"kind\":\"Write\"" : ",\"kind\":\"Read\"");
    host_tl_end();
  }
}

static void __attribute__((constructor)) pg_use(void) {
  io_eff(CID(Pg.open), pg_open_run);
}
