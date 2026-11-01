// Million Users Challenge - C++ submission.
//
// Thread-per-core epoll server. Every worker owns:
//   - its own SO_REUSEPORT listen sockets on PORT and EXTRA_PORT
//   - a pool of libpq connections in pipeline mode (one prepared
//     statement per endpoint, every client request = one query)
//   - an intrusive LRU of client connections for the idle timeout
//
// No shared state between workers: no locks anywhere.
//
// Env: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT
//      THREADS (default: nproc), PG_CONNS (per worker, default 32)
//      IDLE_MS (default 90000, must be > 75 s per SPEC)
//
// Sources: util/json/jwt/http are helpers; api handles requests;
// conn is client io; pg/pgw is the wire-protocol pool; worker is the
// epoll loop; main reads the environment.

#ifndef FEED_SERVER_H
#define FEED_SERVER_H

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <deque>
#include <thread>
#include <vector>

// ------------------------------------------------------------------ config

extern const char* g_dburl;
extern const char* g_secret;
extern size_t g_secret_len;
extern const char* g_host;
extern int g_port;
extern int g_xport;
extern int g_threads;
extern int g_pg_per;
extern int g_uring;
extern uint64_t g_idle_ms;
extern time_t g_start;

// ------------------------------------------------------------------ util

uint64_t now_ms();
[[noreturn]] void die(const char* fmt, ...);
// writes the decimal v ending at e (no NUL); returns the start
char* u64toa(uint64_t v, char* e);

// ------------------------------------------------------------------ core types

enum { EV_LFD = 1, EV_CONN = 2, EV_PG = 3 };
enum { M_GET = 0, M_POST = 1, M_OTHER = 2 };
enum { R_HEALTH, R_FEED, R_GET, R_CREATE, R_LIKE };

struct Conn;
struct Pg;
struct Worker;

enum { RK_NONE = 0, RK_INLINE = 1, RK_HEAP = 2, RK_CONST = 3 };
constexpr int RESP_PAD = 160;   // header room in front of an inline body
constexpr int RESP_CAP = 8192;  // inline body limit; bigger bodies go to heap

struct Req {
  // hot scalar cluster first: request lifecycle touches these on every event
  Worker* w;
  Conn* c;
  Req* c_next;     // Conn.rq chain (response order)
  Req* pg_next;    // Pg.inq chain
  char* resp;      // full response bytes once done
  int64_t id;      // post id (get/like)
  int64_t uid;     // user id (create/like)
  uint32_t rlen;
  uint32_t grp;    // Sync group on its Pg conn: reads share one, writes own one
  int type;
  int status;
  uint16_t blen;
  uint16_t ulen;
  uint8_t done;
  uint8_t on_pg;   // currently inside a Pg queue (inq or sent)
  uint8_t resp_kind;
  uint8_t gz;      // client accepts gzip
  uint8_t held;    // write result held until its pipeline sync (commit point)
  // cold arrays last so scalars share cache lines
  char uname[256];
  char body[2064]; // trimmed post body, <= 500 chars but up to 2000 bytes
  char rarea[RESP_PAD + RESP_CAP];
};

struct Conn {
  int ev;
  int fd;
  Req *rq_head, *rq_tail;
  Conn *lprev, *lnext;  // LRU, front = freshest
  uint64_t last_ms;
  char* rbuf;      // lazily allocated: only while a request spans reads
  uint32_t rpos, rlen, rcap;
  char* wbuf;
  uint32_t woff, wlen, wcap;
  uint8_t closing;      // Connection: close or HTTP/1.0
  uint8_t out_armed;
  // io_uring mode: a zombie Conn must not be recycled while any of its ops
  // can still post a CQE (the user_data points here)
  char* ur_sbuf = nullptr;     // send staging chunk, pinned while send_pend
  uint16_t ur_inflight = 0;    // armed uring ops on this conn
  uint8_t ur_dead = 0;
  uint8_t ur_send_pend = 0;
  uint8_t ur_rxq = 0;          // queued on rxrearm
  uint8_t ur_sendq = 0;        // queued on sendq
};

// one decoded result field: pointer+len into the conn's recv buffer
// (text format; l == -1 means SQL NULL). Valid only inside req_pg_done.
struct PgFld {
  const char* v;
  int32_t l;
};

// a completed result handed to req_pg_done: equivalent of the PGresult
// fields the serializer reads, without libpq's object graph or mallocs.
enum { PGR_TUPLES = 1, PGR_COMMAND = 2, PGR_FATAL = 3 };
struct PgRes {
  int status;
  int nrows;
  int ncols;
  const PgFld* rows;   // nrows*ncols fields, into recv buffer
  const char* tag;     // CommandComplete tag e.g. "INSERT 0 1"
  int taglen;
  char state[6];       // SQLSTATE when status == PGR_FATAL
};

struct Pg {
  int ev = EV_PG;
  uint8_t up = 0;        // set once the conn is connected and prepared
  int fd = -1;
  Req *inq_h = nullptr, *inq_t = nullptr;
  std::deque<Req*> sent;
  Req* held = nullptr;     // write request waiting for its pipeline sync
  uint64_t retry_at = 0;
  uint16_t pgen = 0;       // bumped on pg_down so stale poll CQEs drop
  uint8_t out_armed = 0;
  uint8_t want_flush = 0;  // queued submits pending an end-of-batch pump
  uint8_t pgin_lost = 0;   // uring POLLIN arm failed on a full SQ; retried
  uint8_t pgout_lost = 0;  // same for POLLOUT
  uint8_t wr = 0;          // write-class conn: preferred home for commits so
                         // their fsync stall never delays reads behind them
  uint32_t grp_next = 1;   // id of the Sync group currently being encoded
  uint32_t err_grp = 0;    // Sync group whose remaining results PG skipped
  // wire-protocol io buffers and decode state
  char* in = nullptr;            // recv/parse buffer (grows on demand)
  uint32_t in_len = 0, in_cap = 0;
  char* out = nullptr;           // pending outbound protocol bytes
  uint32_t out_len = 0, out_off = 0, out_cap = 0;
  std::vector<PgFld> flds;       // field table of the result in assembly
  uint16_t fcols = 0;            // column count pinned by the first row
  PgRes res;
};

struct Lfd {
  int ev;
  int fd;
  uint8_t ur_pend = 0;  // accept arm needs a retry (SQ was full)
};

struct io_uring;
struct io_uring_buf_ring;
struct io_uring_sqe;

// io_uring user_data: object pointer | op tag (pointers are >=8 aligned)
enum {
  UD_RECV = 1,   // Conn*  - multishot recv
  UD_SEND = 2,   // Conn*  - send
  UD_PGIN = 3,   // Pg*    - one-shot POLLIN
  UD_PGOUT = 4,  // Pg*    - one-shot POLLOUT
  UD_ACCEPT = 5, // Lfd*   - multishot accept
  UD_MISC = 6,   // cancels etc, ignored
};
constexpr int UR_BGID = 1;  // provided-buffer group id

struct Worker {
  int ep = -1;
  Lfd lfd[2];
  std::vector<Pg> pg;
  Conn* lh = nullptr;
  Conn* lt = nullptr;  // LRU head/tail
  uint64_t now = 0, last_sweep = 0;
  std::vector<Req*> rfree;
  std::vector<Conn*> cfree;    // Conn freelist (accept churn)
  std::vector<char*> bfree;    // 4KB rbuf block freelist (lazy read bufs)
  std::vector<Conn*> zombies;  // closed this batch, freed after
  std::vector<Pg*> pg_flush;   // conns with queued submits, pumped per batch
  char scratch[16384];         // recv target for conns without an rbuf
  void* gzc = nullptr;         // libdeflate compressor, allocated lazily
  uint64_t nconn = 0;
  // io_uring backend state (allocated only when g_uring and setup succeeds)
  uint8_t uring = 0;
  io_uring* ur = nullptr;
  io_uring_buf_ring* br = nullptr;
  char* barena = nullptr;      // provided-buffer slab (demand-paged)
  uint32_t bsz = 0;            // bytes per provided buffer
  uint32_t bmask = 0;          // buf_ring tail mask
  std::vector<uint16_t> bidret;   // consumed bids to hand back to the ring
  std::vector<Conn*> rxrearm;     // conns whose recv died (-ENOBUFS/SQ full)
  std::vector<Conn*> sendq;       // conns waiting for a send SQE slot
  std::vector<char*> sfree;       // send staging chunk freelist
};

// escape into dst (cap bytes) for embedding in a JSON string.
// returns length written, or SIZE_MAX if dst is too small.
size_t jesc(char* d, size_t cap, const char* s, size_t n);

// ------------------------------------------------------------------ small buf

struct Buf {
  char* p;
  char* e;
  bool fail;
  Buf(char* b, size_t n) : p(b), e(b + n), fail(false) {}
  void put(const void* s, size_t n) {
    if ((size_t)(e - p) < n) {
      fail = true;
      return;
    }
    memcpy(p, s, n);
    p += n;
  }
  void puts(const char* s) { put(s, strlen(s)); }
  void ch(char c) {
    if (p >= e) {
      fail = true;
      return;
    }
    *p++ = c;
  }
  void jesc(const char* s, size_t n) {
    size_t r = ::jesc(p, e - p, s, n);
    if (r == SIZE_MAX) {
      fail = true;
      return;
    }
    p += r;
  }
};

// ------------------------------------------------------------------ json cursor
// Minimal scanner: enough to walk a flat object, read a string span, skip any
// value. No allocation; strictness matches what the test suite sends.

struct Jc {
  const char* p;
  const char* e;
};

void jws(Jc& j);
bool jstr(Jc& j, const char** s, uint32_t* n);
bool jskip(Jc& j);
int junesc(char* dst, size_t cap, const char* s, size_t n);
size_t utf8_count(const char* s, size_t n);
int json_body(const char* s, size_t n, char* dst, size_t cap, uint16_t* dlen);

// ------------------------------------------------------------------ jwt / auth

// Authorization header value -> uid/username or error
// returns 0 ok / 1 missing bearer / 2 invalid or expired / 3 invalid payload
int auth_check(const char* v, size_t n, int64_t* uid, char* uname, size_t ucap,
               uint16_t* ulen);

// ------------------------------------------------------------------ http

const char* reason_of(int s);
bool post_id(const char* s, size_t n, int64_t* out);
// returns bytes consumed by one request, or 0 if incomplete / -1 bad
long http_parse(const char* s, size_t n, int* method, const char** path,
                size_t* plen, const char** auth, size_t* authn,
                const char** body, size_t* blen, int* minor, bool* connclose,
                bool* gz);

// ------------------------------------------------------------------ api

Req* req_new(Worker* w, Conn* c);
void req_free(Worker* w, Req* r);
void req_resp(Req* r, int status, const char* body, size_t blen);
void req_err(Req* r, int status, const char* msg);
void req_pg_done(Worker* w, Req* r, PgRes* res);
void dispatch(Worker* w, Conn* c, int method, const char* path, size_t plen,
              const char* authv, size_t authn, const char* body, size_t blen,
              bool gz);

// ------------------------------------------------------------------ conn

void ep_mod(Worker* w, int fd, void* ptr, uint32_t ev);
void conn_drain(Worker* w, Conn* c);
void conn_close(Worker* w, Conn* c);
void conn_on_read(Worker* w, Conn* c);
void on_accept(Worker* w, Lfd* l);

// io_uring backend (conn.cpp): only reached when w->uring
void conn_drain_ur(Worker* w, Conn* c);
void conn_close_ur(Worker* w, Conn* c);
void ur_on_accept(Worker* w, Lfd* l, int res, unsigned flags);
void ur_on_recv(Worker* w, Conn* c, int res, unsigned flags);
void ur_on_send(Worker* w, Conn* c, int res);
void ur_recv_arm(Worker* w, Conn* c);
void ur_kick_send(Worker* w, Conn* c);

// ------------------------------------------------------------------ pg

bool pg_connect(Worker* w, Pg* p);
void pg_submit(Worker* w, Req* r);
void pg_on_read(Worker* w, Pg* p);
void pg_on_write(Worker* w, Pg* p);
void pg_pump(Worker* w, Pg* p);
void pg_down(Worker* w, Pg* p);

// crypto helpers shared by jwt and the pgw scram exchange
struct Sha256;
void sha256_oneshot(const void* data, size_t n, uint8_t out[32]);
void hmac_sha256(const uint8_t* key, size_t klen, const uint8_t* msg,
                 size_t mlen, uint8_t out[32]);
int b64_dec(const char* s, size_t n, uint8_t* out);
int b64_enc(const uint8_t* s, size_t n, char* out);

// ------------------------------------------------------------------ worker

void worker_run(int idx);
void worker_batch_end(Worker* w);

// io_uring backend (uring.cpp)
int ur_setup(Worker* w);
void ur_loop(Worker* w);
io_uring_sqe* ur_sqe(Worker* w);
void ur_accept_arm(Worker* w, Lfd* l);   // (re)arm multishot accept
void ur_pg_in(Worker* w, Pg* p);         // arm one-shot POLLIN
void ur_pg_out_arm(Worker* w, Pg* p);    // arm one-shot POLLOUT

#endif
