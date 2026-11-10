// The C side of host/epoll.bend: route A′'s reactor and HTTP
// (docs/specs/S08-pg-effect-and-interpreter.md). The compiler splices this
// file after the runtime, so the runtime's io_* helpers are in scope
// (vendor/bend/guide/EFFECTS.md). Outside every proof.
//
// One reactor per process; processes started with --workers N, N above 1, share
// the two ports through SO_REUSEPORT and nothing else but the database. The reactor
// owns the listeners and every client socket, in one table indexed by
// descriptor, and two epoll sets: the reactor's (listeners, clients,
// database connections) and one with the database connections only, waited
// on while an open batch (of reads or of writes) has no room, so that
// readable clients do not wake a round that cannot take their requests.
// Bend holds the reactor's descriptor as a Socket handle. No descriptor is
// ever parked in the runtime: Epoll.turn waits in epoll_pwait2 itself.
//
// Per client descriptor: a generation, bumped on every accept, so that
// (conn, gen) names one accepted connection (A6.5); EP_BUSY while a request
// of the connection is in flight, and no further request is lexed until its
// reply is written (A6.7); 512 bytes for the request line and headers, and
// for a request that needs more (a head over 512 bytes, up to --max-head, or
// a body, up to --max-body), a buffer of its size; the unwritten rest of a
// reply, sent on EPOLLOUT. A request the host cannot take is answered by the
// host itself (ep_refuse: 400, 411, 413, 431) and its connection closes.
//
// With --idle-timeout, a lazy timer wheel over the descriptors closes a
// connection that has sent and been sent no byte for that long (ep_idle_run).
//
// With --window-us W (S21, docs/specs/S21-the-wake-window.md), the turn after
// a wake that brought events but did not fill the event array flushes the
// read batch and sleeps until that wake plus W, then takes what arrived with a
// zero wait: one wake serves what came meanwhile (ep_window).
//
// A reply body of EP_GZIP_MIN bytes or more to a request that accepts gzip goes
// out gzipped: by host/gzip.h (--gzip fixed, the default), by libdeflate
// level 1 (--gzip libdeflate) or by ISA-L level 0 (--gzip isal, host/isal.h).
//
// Each round's events go to Bend in one list (host/wire.bend reads them):
// the answers --sync-max held over from the last round first (Synced, or
// Died when the connection fails while they are taken), then Flushed (both
// from host/pg.c, pg_tick), then Synced and Died, then Arrived
// and Gone. Bend folds Policy.round over them and hands the round's commands
// to the next Epoll.turn as a list of Cmd (host/epoll.bend), which the turn
// runs in order before anything else (ep_cmds, docs/specs/S24-the-pure-round.md).
// With --trace every event and every Send is written to the trace file as one
// JSON line; host/pg.c writes Queue and Open lines. With --stats one line of
// counters goes to the stats file every second.
//
// Segments by class (S23, docs/specs/S23-packets-by-class.md): a client socket's TCP_INFO
// segment counts are added to ep.segs when it closes, and at stop for each one still open;
// host/pg.c sums its own sockets (pg_segs). The stats file and the stop line carry both
// classes. With --sock-opts on, the listeners carry TCP_NODELAY, TCP_DEFER_ACCEPT and
// TCP_THIN_LINEAR_TIMEOUTS from before listen() and TCP_QUICKACK 0 from after it, which every
// accepted socket inherits (ep_listen), and a reply that leaves EP_LATE_MS or more after its
// request arrived sets TCP_QUICKACK 0 on its socket first (ep_reply). With --accept-read on as
// well (S28), ep_accept reads each connection it accepts, so that the first request, which
// TCP_DEFER_ACCEPT holds the connection back for, is lexed in the accepting round.

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <libdeflate.h>
#include "host.h"
#include "http.h"
#include "gzip.h"
#include "isal.h"

#define EP_MAX      1024       // events per epoll wait, at most
#define EP_ACCEPTS  256        // accepts per wake; the listeners are level-triggered
#define EP_CAP      (1u << 20) // largest table
#define EP_GZIP_MIN 1024       // the reference compresses bodies of 1 KB or more
#define EP_WAIT_NS  1000000000ull
#define EP_ACCEPT_WAIT_NS 100000000ull  // no accept this long after EMFILE or ENFILE
#define EP_LATE_MS  25         // a reply this late after its request is late (ep_reply)
#define EP_DEFER_S  1          // --sock-opts on: TCP_DEFER_ACCEPT, in seconds (ep_listen)

#define EP_OPEN   1u   // st: a client this table owns
#define EP_QUEUED 2u   // st: on the again ring (kept across a close, see ep_gone)
#define EP_BUSY   4u   // st: a request of this connection is in flight
#define EP_PAUSED 8u   // st: input unwatched, the buffer is full while busy
#define EP_GZIP   16u  // st: the request in flight accepts gzip
#define EP_WHEELED 32u // st: a node of this descriptor is in a wheel bucket (kept across a close)

typedef struct {
  uint32_t cap;
  char     d[];
} EpBig;

typedef struct {
  uint32_t len, off;
  char     d[];
} EpOut;

// --gzip: the encoder of a gzipped reply (ep_gzip)
typedef enum { EP_GZ_FIXED, EP_GZ_LIBDEFLATE, EP_GZ_ISAL } EpGzip;

static const char* const ep_gzip_name[] = {
  [EP_GZ_FIXED] = "fixed", [EP_GZ_LIBDEFLATE] = "libdeflate", [EP_GZ_ISAL] = "isal" };

typedef struct {
  int       epfd, dbfd;
  int       lfd[2];
  uint32_t  cap;    // descriptors 0 .. cap - 1
  uint8_t*  st;
  uint32_t* used;   // request bytes buffered
  char*     buf;    // cap * HTTP_HEAD, mapped with MAP_NORESERVE
  uint32_t* gen;
  EpBig**   big;    // a request's own buffer while it needs more than HTTP_HEAD
  EpOut**   out;    // the unwritten rest of a reply
  uint32_t* ring;   // the again ring: descriptors with bytes to lex
  uint32_t  head, count;
  // --ring lifo (ep_lex_ring): the arrivals lexed in newest-first turns (every --ring-old-th
  // comes from the old end), those turns, the ring's length at them summed, the arrivals taken
  // from the old end, and the largest age in ms the ring's oldest entry had at a lex, since the
  // last stats line and since the start
  uint32_t  ring_k, ring_age_line, ring_age_max;
  uint64_t  ring_turns, ring_len, ring_old;
  uint32_t  req;    // the last request number handed out
  uint64_t  start;
  uint64_t  arrived, sent, dropped, gone;
  uint64_t  accepts;
  uint32_t  busy;   // clients with EP_BUSY: requests in flight
  uint32_t  conns;  // clients with EP_OPEN
  uint64_t  now_ms; // ms since start at this round's wake-up (ep_clock)
  uint64_t  accept_off;  // io_tick() time the listeners are watched again, 0 while they are
  uint64_t  accept_refused, idle_closes, refused[4];  // see ep_refusals
  uint64_t  turns;  // Epoll.turn calls, each one epoll_pwait2 (the stats file's turns)
  uint64_t  late;   // replies written EP_LATE_MS or more after their request's last byte (ep_reply)
  HostSegs  segs;   // the client sockets' segment counts, added at close (host_segs_add)
  // the last epoll_pwait2: the io_tick() it returned at, its events, and whether they filled
  // the array it was given (--window-us, ep_window)
  uint64_t  wake;
  uint32_t  woke;
  bool      full;
  // the ms (mod 2^32) of the last byte either way on each descriptor, wlast[fd]: the idle
  // clock and the age of the request in flight at its reply (ep_reply); --idle-timeout:
  // whead[k % wn] is the first node (descriptor + 1, 0 is none) due at second boundary k;
  // wnext[fd] the next; wk the last boundary taken; wdue the nodes taken and not yet visited
  uint32_t* whead;
  uint32_t* wnext;
  uint32_t* wlast;
  uint32_t  wn, wdue;
  uint64_t  wk;
  EpGzip    gzip;   // --gzip: replies go through gzs (host/gzip.h), gz or gzi
  bool      accept_read;  // --accept-read on: ep_accept reads each connection it accepts (S28)
  struct libdeflate_compressor* gz;  // --gzip libdeflate's compressor, else NULL
  GzState   gzs;    // host/gzip.h's hash table, kept across replies (zeroed: ready)
  struct isal_zstream gzi;  // --gzip isal's stream: 82 KB that no reply reads from another (host/isal.h)
  struct epoll_event evs[EP_MAX];
} EpTable;

static EpTable ep = { .epfd = -1, .dbfd = -1, .lfd = { -1, -1 } };

HostConf host_conf;
FILE*    host_trace;
JBuf     host_tl;

static volatile sig_atomic_t ep_stop;

// --stats: the next whole UTC second (its number and its io_tick time) and
// the counters as the last line left them.
static struct {
  FILE*    f;
  uint64_t sec, next;
  uint64_t accepts, gone, arrived, accept_refused, idle_closes, refused[4], turns, late, ring_turns, ring_len;
  HostSegs cs, ps;  // the client and database classes' sums at the last line
} ep_st;

int host_db_set(void) {
  return ep.dbfd;
}

uint32_t host_uptime(uint64_t now) {
  return (uint32_t)((now - ep.start) / 1000000000ull);
}

// ---- segments by class ----

// The prefix of the kernel's struct tcp_info (include/uapi/linux/tcp.h) up to the segment
// counts, 160 bytes. glibc's <netinet/tcp.h> has these fields on dragonborn but not on the
// lab's Ubuntu 24.04 (glibc 2.39), and <linux/tcp.h> clashes with it. The layout is ABI: the
// kernel only appends fields (.local/receipts/S23/probe/offsets.c prints the offsets).
typedef struct {
  uint8_t  state, ca_state, retransmits, probes, backoff, options, wscale, flags;
  uint32_t rto, ato, snd_mss, rcv_mss, unacked, sacked, lost, retrans, fackets;
  uint32_t last_data_sent, last_ack_sent, last_data_recv, last_ack_recv;
  uint32_t pmtu, rcv_ssthresh, rtt, rttvar, snd_ssthresh, snd_cwnd, advmss, reordering;
  uint32_t rcv_rtt, rcv_space, total_retrans;
  uint64_t pacing_rate, max_pacing_rate, bytes_acked, bytes_received;
  uint32_t segs_out, segs_in, notsent_bytes, min_rtt, data_segs_in, data_segs_out;
} HostTcpInfo;

void host_segs_add(HostSegs* s, int fd) {
  HostTcpInfo t;
  socklen_t   n = sizeof t;
  if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &t, &n) != 0 || n < sizeof t) {
    return;
  }
  s->in += t.segs_in;
  s->out += t.segs_out;
  s->data_in += t.data_segs_in;
  s->data_out += t.data_segs_out;
}

// ---- the trace ----

void host_tl_begin(void) {
  host_tl.n = 0;
  jb_str(&host_tl, "{\"t\":");
  jb_u64(&host_tl, (io_tick() - ep.start) / 1000);
  jb_byte(&host_tl, ',');
}

void host_tl_end(void) {
  jb_str(&host_tl, "}\n");
  fwrite(host_tl.p, 1, host_tl.n, host_trace);
}

// ---- strings from Bend ----

void host_take_str(Env e, Term s, JBuf* b, bool json) {
  static const char hex[] = "0123456789abcdef";
  while (term_aux(s) == CID(SCon)) {
    Term fb[2];
    spare_free(e, cls_fit(2), ctr_take(e, s, 2, fb));
    uint32_t c = (uint32_t)fb[0];
    s = fb[1];
    if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
      c = 0xFFFD;
    }
    if (json && (c < 0x20 || c == '"' || c == '\\')) {
      char t[6] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15] };
      switch (c) {
      case '"': jb_put(b, "\\\"", 2); break;
      case '\\': jb_put(b, "\\\\", 2); break;
      case '\b': jb_put(b, "\\b", 2); break;
      case '\f': jb_put(b, "\\f", 2); break;
      case '\n': jb_put(b, "\\n", 2); break;
      case '\r': jb_put(b, "\\r", 2); break;
      case '\t': jb_put(b, "\\t", 2); break;
      default: jb_put(b, t, 6);
      }
      continue;
    }
    jb_need(b, 4);
    b->n += utf8_put(b->p + b->n, c);
  }
}

// ---- rows from the host's table ----

// Bit 7 of each byte of x that ep_json_text cannot copy as it is: a control (under 0x20), '"',
// '\\' or a byte of 0x80 and over. The three subtractions can also mark a byte above a marked
// one (the borrow), never one below the first: the lowest marked byte is exact.
static inline uint64_t ep_json_odd8(uint64_t x) {
  const uint64_t ones = 0x0101010101010101ull, high = 0x8080808080808080ull;
  uint64_t q = x ^ (ones * '"'), s = x ^ (ones * '\\');
  return (x | ((x - ones * 0x20) & ~x) | ((q - ones) & ~q) | ((s - ones) & ~s)) & high;
}

// Copies u[i ..< n] to o up to the first byte ep_json_text cannot copy as it is, and answers
// its index, n when there is none. The caller has n - i bytes of room at o: a block is stored
// whole before it is looked at, and a block lies within u[i ..< n]. On x86-64 16 bytes at a
// time (the compare is signed, so "less than 0x20" finds the bytes of 0x80 and over with the
// controls), then 8 at a time (this file is little-endian only: host/gzip.h). The bytes left
// after the last whole block go as one more block that ends at n and starts inside the bytes
// already copied (the same bytes stored again, and none of them is marked, as each was looked
// at); 4 to 7 bytes in all are two blocks of 4 that way, and only a run under 4 bytes goes one
// byte at a time.
static inline size_t ep_json_run(const unsigned char* u, size_t i, size_t n, char* o) {
#if defined(__x86_64__)
  if (n - i >= 16) {
    const __m128i quote = _mm_set1_epi8('"'), slash = _mm_set1_epi8('\\'), space = _mm_set1_epi8(0x20);
    for (; n - i >= 16; i += 16, o += 16) {
      __m128i v = _mm_loadu_si128((const __m128i*)(u + i));
      _mm_storeu_si128((__m128i*)o, v);
      __m128i  bad = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, quote), _mm_cmpeq_epi8(v, slash)), _mm_cmplt_epi8(v, space));
      unsigned m   = (unsigned)_mm_movemask_epi8(bad);
      if (m != 0) {
        return i + (size_t)__builtin_ctz(m);
      }
    }
    if (i == n) {
      return n;
    }
    size_t  back = 16 - (n - i);  // 1 to 15
    __m128i v    = _mm_loadu_si128((const __m128i*)(u + i - back));
    _mm_storeu_si128((__m128i*)(o - back), v);
    __m128i  bad = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, quote), _mm_cmpeq_epi8(v, slash)), _mm_cmplt_epi8(v, space));
    unsigned m   = (unsigned)_mm_movemask_epi8(bad);
    return m != 0 ? i - back + (size_t)__builtin_ctz(m) : n;
  }
#endif
  if (n - i >= 8) {
    for (; n - i >= 8; i += 8, o += 8) {
      uint64_t x;
      memcpy(&x, u + i, 8);
      memcpy(o, &x, 8);
      uint64_t m = ep_json_odd8(x);
      if (m != 0) {
        return i + ((size_t)__builtin_ctzll(m) >> 3);
      }
    }
    if (i == n) {
      return n;
    }
    size_t   back = 8 - (n - i);  // 1 to 7
    uint64_t x;
    memcpy(&x, u + i - back, 8);
    memcpy(o - back, &x, 8);
    uint64_t m = ep_json_odd8(x);
    return m != 0 ? i - back + ((size_t)__builtin_ctzll(m) >> 3) : n;
  }
  if (n - i >= 4) {
    uint32_t a, c;
    memcpy(&a, u + i, 4);
    memcpy(&c, u + n - 4, 4);
    memcpy(o, &a, 4);
    memcpy(o + (n - i) - 4, &c, 4);
    uint64_t m = ep_json_odd8((uint64_t)a | (uint64_t)c << 32);
    if (m == 0) {
      return n;
    }
    size_t k = (size_t)__builtin_ctzll(m) >> 3;
    return k < 4 ? i + k : n - 8 + k;
  }
  for (; i < n && u[i] >= 0x20 && u[i] < 0x80 && u[i] != '"' && u[i] != '\\'; i += 1) {
    *o++ = (char)u[i];
  }
  return i;
}

// p[0 ..< n] as host_take_str wrote the String that io_str made of it:
// every ill-formed UTF-8 sequence one U+FFFD (utf8_next decodes as io_str
// does), then escaped as a JSON string body. ASCII runs are copied whole.
static void ep_json_text(JBuf* b, const char* p, size_t n) {
  static const char hex[] = "0123456789abcdef";
  const unsigned char* u = (const unsigned char*)p;
  size_t i = 0;
  while (i < n) {
    jb_need(b, n - i);
    size_t run = ep_json_run(u, i, n, b->p + b->n);
    b->n += run - i;
    i     = run;
    if (i == n) {
      break;
    }
    if (u[i] >= 0x80) {
      uint32_t c = utf8_next(u, n, &i);
      jb_need(b, 4);
      b->n += utf8_put(b->p + b->n, c);
      continue;
    }
    unsigned c = u[i++];
    char     t[6] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15] };
    switch (c) {
    case '"': jb_put(b, "\\\"", 2); break;
    case '\\': jb_put(b, "\\\\", 2); break;
    case '\b': jb_put(b, "\\b", 2); break;
    case '\f': jb_put(b, "\\f", 2); break;
    case '\n': jb_put(b, "\\n", 2); break;
    case '\r': jb_put(b, "\\r", 2); break;
    case '\t': jb_put(b, "\\t", 2); break;
    default: jb_put(b, t, 6);
    }
  }
}

// ---- events ----

enum { EV_WORD, EV_STR, EV_OPEN, EV_CLOSE, EV_ROW };

// EV_WORD: w the word. EV_STR: off and n its bytes in ev_text. EV_ROW: w the
// post's id, off its handle, n its like count.
typedef struct {
  uint8_t  k;
  uint32_t w, off, n;
} EvItem;

static EvItem*  ev_items;
static uint32_t ev_n, ev_cap;
static JBuf     ev_text;
static Term*    ev_list;
static uint32_t ev_list_n, ev_list_cap;
static JBuf     ev_quote;
static uint32_t ev_rows_first, ev_rows_n;

static void ev_item(uint8_t k, uint32_t w, uint32_t off, uint32_t n) {
  if (ev_n == ev_cap) {
    ev_cap   = ev_cap ? ev_cap * 2 : 256;
    ev_items = realloc(ev_items, ev_cap * sizeof *ev_items);
    if (ev_items == NULL) {
      abort();
    }
  }
  ev_items[ev_n++] = (EvItem){ k, w, off, n };
}

void host_ev_begin(void) {
  ev_n      = 0;
  ev_text.n = 0;
  ev_rows_n = 0;
}

void host_ev_rows(uint32_t first, uint32_t n) {
  ev_rows_first = first;
  ev_rows_n     = n;
}

void host_ev_word(uint32_t w) {
  ev_item(EV_WORD, w, 0, 0);
}

void host_ev_str(const char* p, size_t n) {
  ev_item(EV_STR, 0, (uint32_t)ev_text.n, (uint32_t)n);
  jb_put(&ev_text, p, n);
}

void host_ev_open(void) {
  ev_item(EV_OPEN, 0, 0, 0);
}

void host_ev_close(void) {
  ev_item(EV_CLOSE, 0, 0, 0);
}

void host_ev_row(uint32_t id, uint32_t likes, uint32_t h) {
  ev_item(EV_ROW, id, h, likes);
}

static void ev_trace(void) {
  host_tl_begin();
  jb_str(&host_tl, "\"e\":[");
  bool comma = false;
  for (uint32_t i = 0; i < ev_n; i += 1) {
    EvItem* x = &ev_items[i];
    if (x->k != EV_CLOSE && comma) {
      jb_byte(&host_tl, ',');
    }
    switch (x->k) {
    case EV_WORD:
      jb_u64(&host_tl, x->w);
      comma = true;
      break;
    case EV_STR:
      json_quote(&host_tl, ev_text.p + x->off, x->n);
      comma = true;
      break;
    case EV_ROW:  // [id, handle, like_count]: a row's form in the trace (host/wire.bend, Row)
      jb_byte(&host_tl, '[');
      jb_u64(&host_tl, x->w);
      jb_byte(&host_tl, ',');
      jb_u64(&host_tl, x->off);
      jb_byte(&host_tl, ',');
      jb_u64(&host_tl, x->n);
      jb_byte(&host_tl, ']');
      comma = true;
      break;
    case EV_OPEN:
      jb_byte(&host_tl, '[');
      comma = false;
      break;
    case EV_CLOSE:
      jb_byte(&host_tl, ']');
      comma = true;
      break;
    }
  }
  jb_byte(&host_tl, ']');
  if (ev_rows_n > 0) {
    jb_str(&host_tl, ",\"rows\":[");
    for (uint32_t k = 0; k < ev_rows_n; k += 1) {
      uint32_t       h = ev_rows_first + k;
      const HostRow* r = pg_row_at(h);
      jb_str(&host_tl, k > 0 ? ",[" : "[");
      jb_u64(&host_tl, h);
      jb_byte(&host_tl, ',');
      jb_u64(&host_tl, r->req);
      jb_byte(&host_tl, ',');
      json_quote(&host_tl, r->body, r->bn);
      jb_byte(&host_tl, ',');
      json_quote(&host_tl, r->ts, r->tn);
      jb_byte(&host_tl, ',');
      json_quote(&host_tl, r->author, r->an);
      if (host_conf.rows_block) {
        // --rows block: a run's cell holds neither, the reply takes both from the row
        jb_byte(&host_tl, ',');
        jb_u64(&host_tl, r->id);
        jb_byte(&host_tl, ',');
        jb_u64(&host_tl, r->likes);
      }
      jb_byte(&host_tl, ']');
    }
    jb_byte(&host_tl, ']');
  }
  host_tl_end();
}

// Row{id, likes, h, rest}: io_node builds two-field nodes only. The fields lie in
// declared order; the words need no seal, rest gets the one io_node would give it.
static Term ev_row_node(Env e, uint32_t id, uint32_t likes, uint32_t h, Term rest) {
  u64 l = heap_alloc(e, cls_fit(4));
  e.mem[l]     = (Term)id;
  e.mem[l + 1] = (Term)likes;
  e.mem[l + 2] = (Term)h;
  e.mem[l + 3] = io_seal(e, rest, CID(Row));
  return term_ctr(CID(Row), l);
}

// The recorded items as a Wire, built back to front, appended to the list
// this round answers.
void host_ev_end(Env e) {
  if (host_trace != NULL) {
    ev_trace();
  }
  Term     stack[16];
  uint32_t sp  = 0;
  Term     cur = term_pak(CID(End), 0);
  for (uint32_t i = ev_n; i > 0; i -= 1) {
    EvItem* x = &ev_items[i - 1];
    switch (x->k) {
    case EV_WORD:
      cur = io_node(e, CID(Word), (Term)x->w, cur);
      break;
    case EV_STR:
      cur = io_node(e, CID(Str), io_str(e, ev_text.p + x->off, x->n), cur);
      break;
    case EV_ROW:
      cur = ev_row_node(e, x->w, x->n, x->off, cur);
      break;
    case EV_CLOSE:
      if (sp == 16) {
        fprintf(stderr, "bend-epoll: an event nests deeper than 16\n");
        exit(70);
      }
      stack[sp++] = cur;
      cur         = term_pak(CID(End), 0);
      break;
    case EV_OPEN:
      cur = io_node(e, CID(Nest), cur, stack[--sp]);
      break;
    }
  }
  if (ev_list_n == ev_list_cap) {
    ev_list_cap = ev_list_cap ? ev_list_cap * 2 : 256;
    ev_list     = realloc(ev_list, ev_list_cap * sizeof *ev_list);
    if (ev_list == NULL) {
      abort();
    }
  }
  ev_list[ev_list_n++] = cur;
}

// ---- the descriptor sets ----

int host_watch(int fd, uint32_t events, uint64_t data) {
  struct epoll_event ev = { .events = events, .data.u64 = data };
  if (epoll_ctl(ep.epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
    return -1;
  }
  if (epoll_ctl(ep.dbfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
    epoll_ctl(ep.epfd, EPOLL_CTL_DEL, fd, NULL);
    return -1;
  }
  return 0;
}

int host_rewatch(int fd, uint32_t events, uint64_t data) {
  struct epoll_event ev = { .events = events, .data.u64 = data };
  int a = epoll_ctl(ep.epfd, EPOLL_CTL_MOD, fd, &ev);
  int b = epoll_ctl(ep.dbfd, EPOLL_CTL_MOD, fd, &ev);
  return a < 0 || b < 0 ? -1 : 0;
}

void host_unwatch(int fd) {
  epoll_ctl(ep.epfd, EPOLL_CTL_DEL, fd, NULL);
  epoll_ctl(ep.dbfd, EPOLL_CTL_DEL, fd, NULL);
}

// ---- clients ----

static bool ep_client(uint32_t fd) {
  return fd < ep.cap && (ep.st[fd] & EP_OPEN) != 0;
}

static char* ep_bytes(uint32_t fd, uint32_t* cap) {
  if (ep.big[fd] != NULL) {
    *cap = ep.big[fd]->cap;
    return ep.big[fd]->d;
  }
  *cap = HTTP_HEAD;
  return ep.buf + (uint64_t)fd * HTTP_HEAD;
}

static void ep_interest(uint32_t fd) {
  uint32_t ev = (ep.st[fd] & EP_PAUSED) ? 0 : EPOLLIN | EPOLLRDHUP;
  if (ep.out[fd] != NULL) {
    ev |= EPOLLOUT;
  }
  struct epoll_event e = { .events = ev, .data.u64 = fd };
  epoll_ctl(ep.epfd, EPOLL_CTL_MOD, (int)fd, &e);
}

// Each descriptor is on the ring at most once: EP_QUEUED says it is.
static void ep_queue(uint32_t fd) {
  if (ep.st[fd] & EP_QUEUED) {
    return;
  }
  ep.st[fd] |= EP_QUEUED;
  ep.ring[(ep.head + ep.count) % ep.cap] = fd;
  ep.count += 1;
}

// ---- the clock and the idle wheel (--idle-timeout) ----

// Milliseconds since start, taken once per round from the tick the round woke at.
static void ep_clock(uint64_t now) {
  ep.now_ms = (now - ep.start) / 1000000ull;
}

// A byte moved on fd, in either direction: its idle clock restarts, and the age of its request
// in flight counts from here (ep_reply). One store.
static void ep_touch(uint32_t fd) {
  ep.wlast[fd] = (uint32_t)ep.now_ms;
}

// Puts fd's node in the bucket of the first whole second at or after remaining_ms from now.
// The cursor has taken every boundary up to wk, so a node never goes behind it. A bucket k
// % wn holds nodes for boundaries k, k - wn, ...; with wn = timeout + 2 and a cursor that
// keeps up, only boundary k. A node in a bucket before its time costs one extra visit.
static void ep_wheel_add(uint32_t fd, uint32_t remaining_ms) {
  uint64_t k = (ep.now_ms + remaining_ms + 999) / 1000;
  if (k <= ep.wk) {
    k = ep.wk + 1;
  }
  uint32_t b   = (uint32_t)(k % ep.wn);
  ep.wnext[fd] = ep.whead[b];
  ep.whead[b]  = fd + 1;
  ep.st[fd] |= EP_WHEELED;
}

// Nodes are due that no round has visited yet: the next wait must not block.
static bool ep_idle_pending(void) {
  return ep.whead != NULL && (ep.wdue != 0 || ep.wk < ep.now_ms / 1000);
}

// ---- clients, closing ----

// A closed connection: Gone{conn, gen}, its segment counts added to the client class, then the
// descriptor is free. EP_QUEUED stays, because the ring may still hold the descriptor: a
// connection that reuses the number inherits that entry, and the ring never holds it twice.
// EP_WHEELED stays for the same reason: the wheel's node is the descriptor's,
// and its next visit finds the descriptor closed (it drops the node) or open
// again (it decides from that connection's own clock).
static void ep_gone(Env e, uint32_t fd) {
  host_ev_begin();
  host_ev_word(4);
  host_ev_word(fd);
  host_ev_word(ep.gen[fd]);
  host_ev_end(e);
  host_segs_add(&ep.segs, (int)fd);
  close((int)fd);
  free(ep.big[fd]);
  free(ep.out[fd]);
  ep.big[fd]  = NULL;
  ep.out[fd]  = NULL;
  ep.used[fd] = 0;
  ep.busy -= (ep.st[fd] & EP_BUSY) != 0;
  ep.st[fd] &= EP_QUEUED | EP_WHEELED;
  ep.gone += 1;
  ep.conns -= 1;
}

// The wheel's work for this round: the buckets of the whole seconds that have passed since
// the cursor last moved, one at a time, at most EP_MAX nodes in all, so that a second in
// which a million connections expire reaches Bend as a million Gones over many rounds, as
// a mass peer close does. A connection is closed only when it has moved no byte for the
// timeout and owes the host nothing: not while a request of it is with the database (EP_BUSY,
// no reply stuck behind its own socket) or waits to be lexed (on the ring with bytes). Every
// other node goes back into the bucket of its real deadline.
static void ep_idle_run(Env e) {
  if (ep.whead == NULL) {
    return;
  }
  uint64_t passed = ep.now_ms / 1000;
  uint32_t limit  = host_conf.idle_timeout_s * 1000u;
  for (uint32_t budget = EP_MAX; budget > 0;) {
    if (ep.wdue == 0) {
      if (ep.wk >= passed) {
        return;
      }
      ep.wk += 1;
      uint32_t b  = (uint32_t)(ep.wk % ep.wn);
      ep.wdue     = ep.whead[b];
      ep.whead[b] = 0;
      continue;
    }
    uint32_t fd = ep.wdue - 1;
    ep.wdue     = ep.wnext[fd];
    budget -= 1;
    ep.st[fd] &= (uint8_t)~EP_WHEELED;
    if (!(ep.st[fd] & EP_OPEN)) {
      continue;  // the connection this node was made for is gone
    }
    uint32_t age  = (uint32_t)ep.now_ms - ep.wlast[fd];
    bool     owed = ((ep.st[fd] & EP_BUSY) && ep.out[fd] == NULL) || ((ep.st[fd] & EP_QUEUED) && ep.used[fd] > 0);
    if (!owed && age >= limit) {
      ep.idle_closes += 1;
      ep_gone(e, fd);
    } else {
      ep_wheel_add(fd, owed ? limit : limit - age);
    }
  }
}

// ---- clients, accepting ----

// Puts the listeners into the epoll set or takes them out. A listener the set refuses (ENOMEM,
// ENOSPC at the user's max_user_watches) is a server that never accepts again, so the process
// says so and exits 70 rather than run on deaf.
static void ep_listeners(bool watch) {
  for (int i = 0; i < 2; i += 1) {
    if (ep.lfd[i] >= 0) {
      struct epoll_event ev = { .events = EPOLLIN, .data.u64 = HOST_TAG_LSN | (uint64_t)i };
      if (epoll_ctl(ep.epfd, watch ? EPOLL_CTL_ADD : EPOLL_CTL_DEL, ep.lfd[i], &ev) != 0) {
        fprintf(stderr, "bend-epoll: epoll_ctl %s of listener %d: %s\n", watch ? "ADD" : "DEL", i, strerror(errno));
        exit(70);
      }
    }
  }
}

// accept4 found no descriptor (EMFILE, ENFILE). The connection stays in the kernel's queue,
// and the listeners, still readable, would wake every round; so they leave the epoll set for
// EP_ACCEPT_WAIT_NS, and epoll_turn_run puts them back. Closing a client makes room.
static void ep_accept_pause(void) {
  ep.accept_refused += 1;
  ep.accept_off = io_tick() + EP_ACCEPT_WAIT_NS;
  ep_listeners(false);
}

static void ep_read(Env e, uint32_t fd);

// Up to EP_ACCEPTS connections from the listener lfd. With --accept-read on (which needs
// --sock-opts on) each one is read at once (ep_read): TCP_DEFER_ACCEPT hands a connection over
// when its first bytes are in, so its first request goes onto the again ring and this round's
// ep_lex_ring takes it, where otherwise it would wait for the next epoll_pwait2 and, after a
// wake, for --window-us (S28, issue #64; the comparator's newer version reads after accept
// too). A connection the deferral let through without bytes (EP_DEFER_S passed) reads EAGAIN
// and is served when its bytes come; one that failed is Gone in this round. With --accept-read
// off, the default, nothing is read here.
static void ep_accept(Env e, int lfd) {
  if (ep.accept_off != 0) {
    return;  // the listeners are unwatched; this event was read before that
  }
  for (uint32_t i = 0; i < EP_ACCEPTS; i += 1) {
    int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR || errno == ECONNABORTED || errno == EPROTO) {
        continue;  // that connection is gone; the next may be fine
      }
      if (errno == EMFILE || errno == ENFILE) {
        ep_accept_pause();
      }
      return;  // EAGAIN, or out of descriptors
    }
    // --sock-opts on: TCP_NODELAY came with the socket from its listener (ep_listen)
    if (!host_conf.sock_opts) {
      int one = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    struct epoll_event ev = { .events = EPOLLIN | EPOLLRDHUP, .data.u64 = (uint64_t)fd };
    if ((uint32_t)fd >= ep.cap || epoll_ctl(ep.epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
      host_segs_add(&ep.segs, fd);
      close(fd);
      continue;
    }
    ep.st[fd]   = (uint8_t)(EP_OPEN | (ep.st[fd] & (EP_QUEUED | EP_WHEELED)));
    ep.used[fd] = 0;
    ep.gen[fd] += 1;
    ep.accepts += 1;
    ep.conns += 1;
    ep_touch(fd);
    if (ep.whead != NULL && !(ep.st[fd] & EP_WHEELED)) {
      ep_wheel_add(fd, host_conf.idle_timeout_s * 1000u);
    }
    if (ep.accept_read) {
      ep_read(e, (uint32_t)fd);
    }
  }
}

static void ep_pause(uint32_t fd) {
  if ((ep.st[fd] & EP_PAUSED) == 0) {
    ep.st[fd] |= EP_PAUSED;
    ep_interest(fd);
  }
}

static void ep_read(Env e, uint32_t fd) {
  uint32_t cap;
  char*    b    = ep_bytes(fd, &cap);
  uint32_t used = ep.used[fd];
  if (used >= cap) {
    if (ep.st[fd] & EP_BUSY) {
      ep_pause(fd);
    } else {
      ep_queue(fd);
    }
    return;
  }
  ssize_t r;
  do {
    r = recv((int)fd, b + used, cap - used, 0);
  } while (r < 0 && errno == EINTR);
  if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
    ep_gone(e, fd);
    return;
  }
  if (r > 0) {
    used += (uint32_t)r;
    ep.used[fd] = used;
    ep_touch(fd);
  }
  if ((ep.st[fd] & EP_BUSY) == 0) {
    ep_queue(fd);
  } else if (used >= cap) {
    ep_pause(fd);
  }
}

// The reply is written: the connection's next request may be lexed (A6.7).
static void ep_done(uint32_t fd) {
  ep_touch(fd);
  ep.busy -= (ep.st[fd] & EP_BUSY) != 0;
  ep.st[fd] &= (uint8_t)~(EP_BUSY | EP_GZIP);
  if (ep.st[fd] & EP_PAUSED) {
    ep.st[fd] &= (uint8_t)~EP_PAUSED;
    ep_interest(fd);
  }
  if (ep.used[fd] > 0) {
    ep_queue(fd);
  }
}

static void ep_write(uint32_t fd, const char* p, size_t n) {
  ssize_t w;
  do {
    w = send((int)fd, p, n, MSG_NOSIGNAL);
  } while (w < 0 && errno == EINTR);
  if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    ep_done(fd);  // the connection failed; its next read reports it, and it is Gone
    return;
  }
  size_t done = w < 0 ? 0 : (size_t)w;
  if (done > 0) {
    ep_touch(fd);
  }
  if (done == n) {
    ep_done(fd);
    return;
  }
  EpOut* o = malloc(sizeof *o + (n - done));
  if (o == NULL) {
    abort();
  }
  o->len = (uint32_t)(n - done);
  o->off = 0;
  memcpy(o->d, p + done, n - done);
  ep.out[fd] = o;
  ep_interest(fd);
}

static void ep_drain(Env e, uint32_t fd) {
  EpOut* o = ep.out[fd];
  if (o == NULL) {
    return;
  }
  ssize_t w;
  do {
    w = send((int)fd, o->d + o->off, o->len - o->off, MSG_NOSIGNAL);
  } while (w < 0 && errno == EINTR);
  if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    ep_gone(e, fd);
    return;
  }
  o->off += w < 0 ? 0 : (uint32_t)w;
  if (w > 0) {
    ep_touch(fd);
  }
  if (o->off < o->len) {
    return;
  }
  free(o);
  ep.out[fd] = NULL;
  ep_interest(fd);
  ep_done(fd);
}

static void ep_event(Env e, uint32_t fd, uint32_t ev) {
  if (!ep_client(fd)) {
    return;
  }
  if (ev & (EPOLLERR | EPOLLHUP)) {
    ep_gone(e, fd);
    return;
  }
  if (ev & EPOLLOUT) {
    ep_drain(e, fd);
    if (!ep_client(fd)) {
      return;
    }
  }
  if (ev & (EPOLLIN | EPOLLRDHUP)) {
    ep_read(e, fd);
  }
}

// ---- requests ----

// What the host answers by itself when a request cannot become an Arrived: the status
// http_lex gives (negated), its reason phrase and its JSON error. The Bun reference answers
// 431 and 413 as well; 411 is ours (Bun decodes chunked bodies), 400 is S05's.
static const struct {
  uint32_t    status;
  const char *reason, *error;
} ep_refusals[4] = {
  { 400, "Bad Request", "bad request" },
  { 411, "Length Required", "length required" },
  { 413, "Content Too Large", "content too large" },
  { 431, "Request Header Fields Too Large", "request header fields too large" },
};

// Answers the connection with one of ep_refusals, counts it, writes the trace's host_reply
// note (the policy sees no Arrived for it, only the Gone that follows), and closes. The
// reply is one send: a fresh connection's buffer holds it, and a client that does not read
// is not worth a state of its own. A close with unread bytes in the socket sends RST, which
// can take the reply back from a client that was still sending its body; so the bytes that
// have already arrived are read and dropped first (at most 16 reads of 64 KB). Bytes that
// arrive after the close still reset the connection.
static void ep_refuse(Env e, uint32_t fd, uint32_t status) {
  uint32_t i = 0;
  while (i < 4 && ep_refusals[i].status != status) {
    i += 1;
  }
  if (i == 4) {
    fprintf(stderr, "bend-epoll: no host reply for status %u\n", status);
    exit(70);
  }
  char out[320];
  int  n = snprintf(out, sizeof out,
    "HTTP/1.1 %u %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n"
    "{\"error\":\"%s\"}", status, ep_refusals[i].reason, strlen(ep_refusals[i].error) + 12, ep_refusals[i].error);
  send((int)fd, out, (size_t)n, MSG_NOSIGNAL);
  static char sink[65536];
  for (int k = 0; k < 16 && recv((int)fd, sink, sizeof sink, 0) > 0; k += 1) {
  }
  ep.refused[i] += 1;
  if (host_trace != NULL) {
    host_tl_begin();
    jb_str(&host_tl, "\"note\":\"host_reply\",\"status\":");
    jb_u64(&host_tl, status);
    jb_str(&host_tl, ",\"conn\":");
    jb_u64(&host_tl, fd);
    jb_str(&host_tl, ",\"gen\":");
    jb_u64(&host_tl, ep.gen[fd]);
    host_tl_end();
  }
  ep_gone(e, fd);
}

static JBuf ep_name, ep_text;

// The descriptor needs need bytes for its request: a buffer of that size.
static void ep_grow(uint32_t fd, uint32_t need) {
  EpBig* g = malloc(sizeof *g + need);
  if (g == NULL) {
    abort();
  }
  uint32_t cap;
  char*    b = ep_bytes(fd, &cap);
  g->cap     = need;
  memcpy(g->d, b, ep.used[fd]);
  free(ep.big[fd]);
  ep.big[fd] = g;
}

// Drops the first len bytes of the descriptor's buffer.
static void ep_consume(uint32_t fd, uint32_t len) {
  uint32_t cap;
  char*    b    = ep_bytes(fd, &cap);
  uint32_t rest = ep.used[fd] - len;
  if (ep.big[fd] != NULL && rest <= HTTP_HEAD) {
    memcpy(ep.buf + (uint64_t)fd * HTTP_HEAD, b + len, rest);
    free(ep.big[fd]);
    ep.big[fd] = NULL;
  } else {
    memmove(b, b + len, rest);
  }
  ep.used[fd] = rest;
}

// Lexes one request of fd. True when it arrived: Arrived{req, conn, gen, r}
// is recorded and the connection is busy until its reply is written.
static bool ep_lex(Env e, uint32_t fd) {
  uint8_t s = ep.st[fd];
  if (!(s & EP_OPEN) || (s & EP_BUSY) || ep.out[fd] != NULL || ep.used[fd] == 0) {
    return false;
  }
  uint32_t cap, need;
  char*    b = ep_bytes(fd, &cap);
  HttpReq  r;
  int      got = http_lex(b, ep.used[fd], cap, host_conf.max_head, host_conf.max_body, &r, &need);
  if (got < 0) {
    ep_refuse(e, fd, (uint32_t)-got);
    return false;
  }
  if (got == 0) {
    if (need > 0) {
      ep_grow(fd, need);
    }
    return false;
  }
  uint32_t uid  = 0;
  uint32_t auth = AUTH_NO_BEARER;
  uint32_t body = r.clen == 0 ? BODY_NONE_SENT : BODY_NOT_JSON;
  ep_name.n = 0;
  ep_text.n = 0;
  if (r.route == 3 || r.route == 4) {
    auth = http_auth(&r, &uid, &ep_name);
  }
  if (r.route == 3 && auth == AUTH_USER) {
    body = http_body(b + r.head, (size_t)r.clen, &ep_text);
  }
  ep.req = ep.req == 0xFFFFFFFFu ? 1 : ep.req + 1;
  ep.st[fd] |= EP_BUSY;
  ep.busy += 1;
  if (r.gzip) {
    ep.st[fd] |= EP_GZIP;
  }
  host_ev_begin();
  host_ev_word(0);
  host_ev_word(ep.req);
  host_ev_word(fd);
  host_ev_word(ep.gen[fd]);
  host_ev_word(r.route);
  host_ev_word(r.id);
  host_ev_word(auth);
  host_ev_word(uid);
  host_ev_str(ep_name.p, ep_name.n);
  host_ev_word(body);
  host_ev_str(ep_text.p, ep_text.n);
  host_ev_end(e);
  ep_consume(fd, r.head + (uint32_t)r.clen);
  ep.arrived += 1;
  return true;
}

// At most limit arrivals from the ring, each descriptor looked at once: oldest first.
//
// --ring lifo (S35, docs/specs/S35-newest-first-under-a-backlog.md; without the flag, and with
// --ring fifo, newest is false and the loop takes the head every time, as before the flag).
// While the ring's oldest entry has waited --ring-age ms or more, the turn takes the newest
// entry instead, and every --ring-old-th arrival of such turns from the old end (N, 8 without
// the flag): an entry with d older entries before it is lexed by the N * (d + 1)-th arrival
// after it, at the latest. The age is the turn's time less the head's wlast: the time its
// request's bytes were read when the request came alone; the time of its connection's last
// reply when it came pipelined behind another (ep_done queues it then); the new connection's
// time when a reused descriptor inherited the entry. The last two read the ring as younger
// than it is while such an entry is the head, which costs a turn that takes the head first,
// as fifo does, and then the next head is read. The reason for the order is the peer's
// delayed-acknowledgement timer (40 ms): a request that has waited past it has cost its extra
// segment already and loses nothing more by waiting, while a fresh one can still be answered
// before the timer. Oldest first makes every request of a backlog wait the whole backlog, so
// in overload every reply is late. The ring holds descriptors, one entry a connection
// (EP_QUEUED), so a connection's own requests keep their order; across connections no law and
// no A6 obligation speaks of the order of arrivals. An entry taken from either end has its
// EP_QUEUED cleared, as before.
static void ep_lex_ring(Env e, uint32_t limit) {
  uint32_t k      = 0;
  bool     newest = false;
  if (host_conf.ring_lifo && ep.count > 0 && limit > 0) {
    uint32_t age     = (uint32_t)ep.now_ms - ep.wlast[ep.ring[ep.head]];
    ep.ring_age_line = age > ep.ring_age_line ? age : ep.ring_age_line;
    ep.ring_age_max  = age > ep.ring_age_max ? age : ep.ring_age_max;
    newest           = age >= host_conf.ring_age_ms;
    ep.ring_turns   += newest;
    ep.ring_len     += newest ? ep.count : 0;
  }
  for (uint32_t look = ep.count; look > 0 && k < limit; look -= 1) {
    bool     old = !newest || ep.ring_k % host_conf.ring_old == host_conf.ring_old - 1;
    uint32_t fd;
    ep.count -= 1;
    if (old) {
      fd      = ep.ring[ep.head];
      ep.head = (ep.head + 1) % ep.cap;
    } else {
      fd = ep.ring[(ep.head + ep.count) % ep.cap];
    }
    ep.st[fd] &= (uint8_t)~EP_QUEUED;
    if (ep_lex(e, fd)) {
      k += 1;
      if (newest) {
        ep.ring_k   += 1;
        ep.ring_old += old;
      }
    }
  }
}

// ---- replies ----

static JBuf ep_body, ep_resp, ep_gzb;

static const char* ep_reason(uint32_t status) {
  switch (status) {
  case 200: return "OK";
  case 201: return "Created";
  case 400: return "Bad Request";
  case 401: return "Unauthorized";
  case 404: return "Not Found";
  case 500: return "Internal Server Error";
  case 503: return "Service Unavailable";
  default: return "Unknown";
  }
}

// Whether (conn, gen) is the connection whose request is in flight (A6.6).
static bool ep_owns(uint32_t conn, uint32_t gen) {
  return ep_client(conn) && ep.gen[conn] == gen && (ep.st[conn] & EP_BUSY) != 0;
}

// The room for a gzip member of at most bound bytes, at the start of ep_gzb.
static uint8_t* ep_gz_room(size_t bound) {
  ep_gzb.n = 0;
  jb_need(&ep_gzb, bound);
  return (uint8_t*)ep_gzb.p;
}

// The gzip member of p[0 .. n - 1] in ep_gzb, by the encoder --gzip names, each with the room
// its own bound asks for; the member's length, or 0 when the encoder has none.
static size_t ep_gzip(const uint8_t* p, size_t n) {
  size_t bound;
  switch (ep.gzip) {
  case EP_GZ_FIXED:
    bound = gz_bound(n);
    return gz_compress(&ep.gzs, p, n, ep_gz_room(bound), bound);
  case EP_GZ_LIBDEFLATE:
    bound = libdeflate_gzip_compress_bound(ep.gz, n);
    return libdeflate_gzip_compress(ep.gz, p, n, ep_gz_room(bound), bound);
  case EP_GZ_ISAL:
    bound = gi_bound(n);
    return gi_compress(&ep.gzi, p, n, ep_gz_room(bound), bound);
  }
  return 0;
}

// Writes ep_body with status to conn, gzipped when the request accepts gzip
// and the body has 1 KB or more (the reference's rule), by ep_gzip. An encoder
// that answers 0 leaves the body as it is.
//
// A reply that leaves EP_LATE_MS or more after its request's last byte arrived is late (the
// age is the round's wake, ep.now_ms, less wlast[conn], which nothing moves between the request
// and its reply on a connection that waits for its reply). Its socket's delayed-ACK timer
// (40 ms at the least) may have fired and taken the socket out of delayed-ACK mode, and then
// the connection's next request would be ACKed at once in a segment of its own; with
// --sock-opts on, TCP_QUICKACK 0 before the write puts the socket back, and a late reply costs
// one pure ACK instead of two (.local/receipts/S23/probe/late.c).
static void ep_reply(uint32_t conn, uint32_t status) {
  if ((uint32_t)ep.now_ms - ep.wlast[conn] >= EP_LATE_MS) {
    ep.late += 1;
    if (host_conf.sock_opts) {
      int zero = 0;
      setsockopt((int)conn, IPPROTO_TCP, TCP_QUICKACK, &zero, sizeof zero);
    }
  }
  const char* p  = ep_body.p;
  size_t      n  = ep_body.n;
  bool        gz = false;
  if ((ep.st[conn] & EP_GZIP) && n >= EP_GZIP_MIN) {
    size_t z = ep_gzip((const uint8_t*)p, n);
    if (z > 0) {
      p  = ep_gzb.p;
      n  = z;
      gz = true;
    }
  }
  ep_resp.n = 0;
  jb_str(&ep_resp, "HTTP/1.1 ");
  jb_u64(&ep_resp, status);
  jb_byte(&ep_resp, ' ');
  jb_str(&ep_resp, ep_reason(status));
  jb_str(&ep_resp, "\r\nContent-Type: application/json\r\n");
  if (gz) {
    jb_str(&ep_resp, "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n");
  }
  jb_str(&ep_resp, "Content-Length: ");
  jb_u64(&ep_resp, n);
  jb_str(&ep_resp, "\r\n\r\n");
  jb_put(&ep_resp, p, n);
  ep_write(conn, ep_resp.p, ep_resp.n);
}

// One Send: written when (conn, gen) still owns the request, dropped
// otherwise; the trace gets the reply either way.
static void ep_send(uint32_t req, uint32_t conn, uint32_t gen, uint32_t status) {
  bool own = ep_owns(conn, gen);
  if (own) {
    ep_reply(conn, status);
    ep.sent += 1;
  } else {
    ep.dropped += 1;
  }
  if (host_trace != NULL) {
    host_tl_begin();
    jb_str(&host_tl, "\"c\":\"Send\",\"req\":");
    jb_u64(&host_tl, req);
    jb_str(&host_tl, ",\"conn\":");
    jb_u64(&host_tl, conn);
    jb_str(&host_tl, ",\"gen\":");
    jb_u64(&host_tl, gen);
    jb_str(&host_tl, ",\"status\":");
    jb_u64(&host_tl, status);
    jb_str(&host_tl, ",\"body\":");
    jb_put(&host_tl, ep_body.p, ep_body.n);
    jb_str(&host_tl, own ? ",\"wrote\":1" : ",\"wrote\":0");
    host_tl_end();
  }
}

// ---- the Send commands ----
// One per Reply shape, as ep_cmds hands them over (host/epoll.bend, Cmd): f
// holds the command's fields in declared order, req, conn and gen first.

static void ep_send_error(Env e, const Term* f) {
  ep_body.n = 0;
  jb_str(&ep_body, "{\"error\":\"");
  host_take_str(e, f[4], &ep_body, true);
  jb_str(&ep_body, "\"}");
  ep_send((uint32_t)f[0], (uint32_t)f[1], (uint32_t)f[2], (uint32_t)f[3]);
}

static void ep_send_health(const Term* f) {
  ep_body.n = 0;
  jb_str(&ep_body, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
  jb_u64(&ep_body, (uint32_t)f[3]);
  jb_byte(&ep_body, '}');
  ep_send((uint32_t)f[0], (uint32_t)f[1], (uint32_t)f[2], 200);
}

static void ep_send_degraded(const Term* f) {
  ep_body.n = 0;
  jb_str(&ep_body, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"database unavailable\"}");
  ep_send((uint32_t)f[0], (uint32_t)f[1], (uint32_t)f[2], 503);
}

static void ep_send_liked(const Term* f) {
  ep_body.n = 0;
  jb_str(&ep_body, (uint32_t)f[4] ? "{\"liked\":true,\"already_liked\":true,\"post_id\":"
                                   : "{\"liked\":true,\"already_liked\":false,\"post_id\":");
  jb_u64(&ep_body, (uint32_t)f[5]);
  jb_byte(&ep_body, '}');
  ep_send((uint32_t)f[0], (uint32_t)f[1], (uint32_t)f[2], (uint32_t)f[3]);
}

// A wrong handle can only be a host bug (no Policy State holds a row:
// .local/receipts/S09/notes.md, item 2), and after it the row table is
// suspect, so the process stops as a Queue for a closed batch does.
static void ep_row_stop(const char* who, uint32_t h, uint32_t req, uint32_t id, const char* why) {
  fprintf(stderr, "bend-epoll: %s: handle %u for req %u, post %u: %s\n", who, h, req, id, why);
  exit(70);
}

// One post object: body, created_at and author from the row r. id and like_count are the
// caller's: with --rows each the ones Bend gives beside the handle, with --rows block the
// row's own (ep_send_posts).
static void ep_post(JBuf* b, const HostRow* r, uint32_t id, uint32_t likes) {
  jb_str(b, "{\"id\":");
  jb_u64(b, id);
  jb_str(b, ",\"body\":\"");
  ep_json_text(b, r->body, r->bn);
  jb_str(b, "\",\"created_at\":\"");
  ep_json_text(b, r->ts, r->tn);
  jb_str(b, "\",\"author\":\"");
  ep_json_text(b, r->author, r->an);
  jb_str(b, "\",\"like_count\":");
  jb_u64(b, likes);
  jb_byte(b, '}');
}

// f: req, conn, gen, shape, rows (Http.posts, a SendPosts). rows is a list of
// Row cells in reply order, End after the last (host/main.bend, pack_rows),
// taken apart as it is read. With --rows each, and for a single post in either
// mode, a cell is Row{id, likes, h}, one per post: the handle must name a row
// of this round, issued for req, whose post id is the id beside it (pg_row),
// and the post is rendered with the cell's id and like count. With --rows
// block a feed's list is one cell, Row{count, 0, first handle}: the run must
// be the whole of req's answer (pg_run) and the list's only cell, and each
// post is rendered from its row, id and like count included. Shape 0 is Feed (200,
// {"posts":[..]}), 1 is One (200, {"post":..}), and One takes exactly one row:
// any other count, or another shape, answers 500. Precondition (as for
// host_take_str): rows is a value, every node a constructor. main.bend builds
// only Row and End, so any other node is a host bug and stops the process as a
// wrong handle does.
static void ep_send_posts(Env e, const Term* f) {
  uint32_t req = (uint32_t)f[0], shape = (uint32_t)f[3], status = 200, n = 0;
  Term     rows = f[4];
  ep_body.n = 0;
  jb_str(&ep_body, shape == 0 ? "{\"posts\":[" : "{\"post\":");
  while (term_aux(rows) == CID(Row)) {
    Term fb[4];
    spare_free(e, cls_fit(4), ctr_take(e, rows, 4, fb));
    uint32_t    id = (uint32_t)fb[0], likes = (uint32_t)fb[1], h = (uint32_t)fb[2];
    const char* why = NULL;
    if (host_conf.rows_block && shape == 0) {
      // --rows block (S33): the cell is a feed's whole answer, id its count of rows and h the
      // first one's handle. The run must be the whole of req's answer (pg_run), and the one
      // cell of the list; each post's id and like count are its row's own.
      uint32_t       at = h;
      const HostRow* r  = n == 0 ? pg_run(h, id, req, &why, &at) : NULL;
      if (r == NULL) {
        ep_row_stop("Http.posts", at, req, 0, n == 0 ? why : "a second run in one answer");
      }
      for (uint32_t k = 0; k < id; k += 1) {
        if (k > 0) {
          jb_byte(&ep_body, ',');
        }
        ep_post(&ep_body, &r[k], r[k].id, r[k].likes);
      }
      n    = id;
      rows = fb[3];
      continue;
    }
    const HostRow* r = pg_row(h, req, id, &why);
    if (r == NULL) {
      ep_row_stop("Http.posts", h, req, id, why);
    }
    if (n > 0) {
      jb_byte(&ep_body, ',');
    }
    ep_post(&ep_body, r, id, likes);
    n   += 1;
    rows = fb[3];
  }
  if (term_aux(rows) != CID(End)) {
    fprintf(stderr, "bend-epoll: Http.posts: req %u: a node of its rows is neither Row nor End\n", req);
    exit(70);
  }
  if (shape == 0) {
    jb_str(&ep_body, "]}");
  } else if (shape == 1 && n == 1) {
    jb_byte(&ep_body, '}');
  } else {
    status    = 500;
    ep_body.n = 0;
    jb_str(&ep_body, "{\"error\":\"internal server error\"}");
  }
  ep_send(req, (uint32_t)f[1], (uint32_t)f[2], status);
}

// Created (201): the request's own text and author, the id and created_at
// its InsertPost returned, the like_count respond gives (SPEC.md:47: 0).
static void ep_send_created(Env e, const Term* f) {
  ep_body.n = 0;
  jb_str(&ep_body, "{\"post\":{\"id\":");
  jb_u64(&ep_body, (uint32_t)f[3]);
  jb_str(&ep_body, ",\"body\":\"");
  host_take_str(e, f[4], &ep_body, true);
  jb_str(&ep_body, "\",\"created_at\":\"");
  host_take_str(e, f[5], &ep_body, true);
  jb_str(&ep_body, "\",\"author\":\"");
  host_take_str(e, f[6], &ep_body, true);
  jb_str(&ep_body, "\",\"like_count\":");
  jb_u64(&ep_body, (uint32_t)f[7]);
  jb_str(&ep_body, "}}");
  ep_send((uint32_t)f[0], (uint32_t)f[1], (uint32_t)f[2], 201);
}

// ---- the commands of a round ----

// A command that is no Cmd constructor, or a list node that is neither Con nor
// Nil, can only be a host bug (host/main.bend builds the list from Cmd and
// List alone), and the process stops as for a wrong handle.
static void ep_cmd_stop(const char* who, const char* what) {
  fprintf(stderr, "bend-epoll: %s: %s\n", who, what);
  exit(70);
}

// One command, taken apart as it is read (ctr_take frees its node): a Send*
// renders and sends its reply as ep_send does, Queue and Open go to host/pg.c.
// The arities are those of host/epoll.bend's Cmd.
static void ep_cmd(Env e, Term c, const char* who) {
  Term     f[8];
  uint32_t aux = term_aux(c);
  if (aux == CID(SendError)) {
    spare_free(e, cls_fit(5), ctr_take(e, c, 5, f));
    ep_send_error(e, f);
  } else if (aux == CID(SendHealth)) {
    spare_free(e, cls_fit(4), ctr_take(e, c, 4, f));
    ep_send_health(f);
  } else if (aux == CID(SendDegraded)) {
    spare_free(e, cls_fit(3), ctr_take(e, c, 3, f));
    ep_send_degraded(f);
  } else if (aux == CID(SendLiked)) {
    spare_free(e, cls_fit(6), ctr_take(e, c, 6, f));
    ep_send_liked(f);
  } else if (aux == CID(SendPosts)) {
    spare_free(e, cls_fit(5), ctr_take(e, c, 5, f));
    ep_send_posts(e, f);
  } else if (aux == CID(SendCreated)) {
    spare_free(e, cls_fit(8), ctr_take(e, c, 8, f));
    ep_send_created(e, f);
  } else if (aux == CID(Queue)) {
    spare_free(e, cls_fit(7), ctr_take(e, c, 7, f));
    pg_queue(e, f);
  } else if (aux == CID(Open)) {
    spare_free(e, cls_fit(2), ctr_take(e, c, 2, f));
    pg_open_batch((uint32_t)f[0], (uint32_t)f[1]);
  } else {
    ep_cmd_stop(who, "a command is no Cmd");
  }
}

// The last round's commands, in list order, each once: the order Policy.round
// answered them, which is step order (PROOF.bend, Round.append). Precondition
// (as for host_take_str): xs is a value, every node a constructor.
static void ep_cmds(Env e, Term xs, const char* who) {
  while (term_aux(xs) == CID(Con)) {
    Term f[2];
    spare_free(e, cls_fit(2), ctr_take(e, xs, 2, f));
    ep_cmd(e, f[0], who);
    xs = f[1];
  }
  if (term_aux(xs) != CID(Nil)) {
    ep_cmd_stop(who, "a node of the command list is neither Con nor Nil");
  }
}

// ---- Epoll.open ----

static bool ep_num(const char* v, uint32_t lo, uint32_t hi, uint32_t* out) {
  char*              end;
  unsigned long long x = strtoull(v, &end, 10);
  if (*v < '0' || *v > '9' || *end != 0 || x < lo || x > hi) {
    return false;
  }
  *out = (uint32_t)x;
  return true;
}

// name.K, for the files of worker K: workers share a directory, never a file.
static const char* ep_suffix(const char* name, uint32_t k) {
  size_t n = strlen(name) + 12;
  char*  p = malloc(n);
  if (p == NULL) {
    abort();
  }
  snprintf(p, n, "%s.%u", name, k);
  return p;
}

// The flags of host/main.bend's usage line; NULL, or what is wrong.
static const char* ep_flags(void) {
  HostConf c = { .port = 8080, .db = DB_PG, .pool = 64, .batch_max = 64,
    .batch_wait_us = 500, .db_timeout_ms = 5000, .max = 64, .fuel = 0xFFFFFFFFu,
    .max_head = 4096, .max_body = 16384, .idle_timeout_s = 0,
    .window_us = 2000,  // the FLIP2 run (bench/results/lab/FLIP2/): with the window and the encoder
                        // A' holds 0.998 of the comparator's rate at 33 task us/req, p99 23-25 ms
    .workers = 1, .golden = "vendor/million-users-challenge/test/golden",
    .sock_opts = false,  // --sock-opts off: the S23 lab A/B (bench/results/lab/S23-ab/)
    .ring_age_ms = 10,   // --ring-age 10: a quarter of the peer's 40 ms timer; --ring fifo is the default
    .ring_old = 8 };     // --ring-old 8: an eighth of a newest-first turn's arrivals come from the old end
  bool has_k = false, has_n = false, has_ws = false, has_ww = false, has_rb = false;
  EpGzip gzip = EP_GZ_FIXED;  // --gzip fixed: the S22 lab A/B (bench/results/lab/S22-ab/)
  bool accept_read = false;  // --accept-read off: until the lab pair of issue #64 decides
  for (int i = 1; i < io_argc; i += 2) {
    const char* k = io_argv[i];
    const char* v = i + 1 < io_argc ? io_argv[i + 1] : NULL;
    if (v == NULL) {
      return "every flag takes a value";
    }
    if (!strcmp(k, "--port")) {
      if (!ep_num(v, 1, 65535, &c.port)) return "--port: 1 to 65535";
    } else if (!strcmp(k, "--extra-port")) {
      if (!ep_num(v, 0, 65535, &c.extra_port)) return "--extra-port: 0 (none) to 65535";
    } else if (!strcmp(k, "--db")) {
      if (!strcmp(v, "pg")) c.db = DB_PG;
      else if (!strcmp(v, "canned")) c.db = DB_CANNED;
      else return "--db: pg or canned";
    } else if (!strcmp(k, "--pool")) {
      if (!ep_num(v, 1, 511, &c.pool)) return "--pool: 1 to 511";
    } else if (!strcmp(k, "--batch-max")) {
      if (!ep_num(v, 1, 1024, &c.batch_max)) return "--batch-max: 1 to 1024";
    } else if (!strcmp(k, "--read-batch-max")) {
      if (!ep_num(v, 1, 1024, &c.read_batch_max)) return "--read-batch-max: 1 to --batch-max";
      has_rb = true;
    } else if (!strcmp(k, "--sync-max")) {
      if (!ep_num(v, 0, 1024, &c.sync_max)) return "--sync-max: 0 (no bound) to 1024";
    } else if (!strcmp(k, "--batch-wait")) {
      if (!ep_num(v, 0, 1000000, &c.batch_wait_us)) return "--batch-wait: 0 to 1000000 (µs)";
    } else if (!strcmp(k, "--write-slots")) {
      if (!ep_num(v, 0, 510, &c.write_slots)) return "--write-slots: 0 (no reservation) to --pool - 1";
      has_ws = true;
    } else if (!strcmp(k, "--write-wait")) {
      if (!ep_num(v, 0, 1000000, &c.write_wait_us)) return "--write-wait: 0 to 1000000 (µs)";
      has_ww = true;
    } else if (!strcmp(k, "--db-timeout")) {
      if (!ep_num(v, 100, 600000, &c.db_timeout_ms)) return "--db-timeout: 100 to 600000 (ms)";
    } else if (!strcmp(k, "--max")) {
      if (!ep_num(v, 1, EP_MAX, &c.max)) return "--max: 1 to 1024";
    } else if (!strcmp(k, "--window-us")) {
      if (!ep_num(v, 0, 1000000, &c.window_us)) return "--window-us: 0 (off) to 1000000 (µs)";
    } else if (!strcmp(k, "--fuel")) {
      if (!ep_num(v, 1, 0xFFFFFFFFu, &c.fuel)) return "--fuel: 1 to 4294967295";
    } else if (!strcmp(k, "--worker")) {
      if (!ep_num(v, 0, 511, &c.worker)) return "--worker: 0 to 511";
      has_k = true;
    } else if (!strcmp(k, "--workers")) {
      if (!ep_num(v, 1, 512, &c.workers)) return "--workers: 1 to 512";
      has_n = true;
    } else if (!strcmp(k, "--max-head")) {
      if (!ep_num(v, HTTP_HEAD, HTTP_HEAD_MAX, &c.max_head)) return "--max-head: 512 to 65536 (bytes)";
    } else if (!strcmp(k, "--max-body")) {
      if (!ep_num(v, 0, HTTP_BODY_MAX, &c.max_body)) return "--max-body: 0 to 999999 (bytes)";
    } else if (!strcmp(k, "--idle-timeout")) {
      if (!ep_num(v, 0, 86400, &c.idle_timeout_s)) return "--idle-timeout: 0 (off) to 86400 (seconds)";
    } else if (!strcmp(k, "--trace")) {
      c.trace = v;
    } else if (!strcmp(k, "--stats")) {
      c.stats = v;
    } else if (!strcmp(k, "--golden")) {
      c.golden = v;
    } else if (!strcmp(k, "--gzip")) {
      if (!strcmp(v, "fixed")) gzip = EP_GZ_FIXED;
      else if (!strcmp(v, "libdeflate")) gzip = EP_GZ_LIBDEFLATE;
      else if (!strcmp(v, "isal")) gzip = EP_GZ_ISAL;
      else return "--gzip: fixed, libdeflate or isal";
    } else if (!strcmp(k, "--sock-opts")) {
      if (!strcmp(v, "on")) c.sock_opts = true;
      else if (!strcmp(v, "off")) c.sock_opts = false;
      else return "--sock-opts: on or off";
    } else if (!strcmp(k, "--accept-read")) {
      if (!strcmp(v, "on")) accept_read = true;
      else if (!strcmp(v, "off")) accept_read = false;
      else return "--accept-read: on or off";
    } else if (!strcmp(k, "--rows")) {
      if (!strcmp(v, "each")) c.rows_block = false;
      else if (!strcmp(v, "block")) c.rows_block = true;
      else return "--rows: each or block";
    } else if (!strcmp(k, "--ring")) {
      if (!strcmp(v, "fifo")) c.ring_lifo = false;
      else if (!strcmp(v, "lifo")) c.ring_lifo = true;
      else return "--ring: fifo or lifo";
    } else if (!strcmp(k, "--ring-age")) {
      if (!ep_num(v, 0, 60000, &c.ring_age_ms)) return "--ring-age: 0 to 60000 (ms)";
    } else if (!strcmp(k, "--ring-old")) {
      if (!ep_num(v, 1, 1024, &c.ring_old)) return "--ring-old: 1 to 1024";
    } else {
      return "unknown flag (bench/servers/bend-epoll/README.md lists them)";
    }
  }
  if (c.extra_port == c.port) {
    return "--extra-port equals --port";
  }
  // Without TCP_DEFER_ACCEPT a connection is accepted on the handshake's last ACK, before its
  // first bytes: the read in the accepting round would find nothing.
  if (accept_read && !c.sock_opts) {
    return "--accept-read on needs --sock-opts on";
  }
  if (has_k != has_n) {
    return "--worker and --workers go together";
  }
  if (c.worker >= c.workers) {
    return "--worker: 0 to --workers - 1";
  }
  // The last --write-slots slots of the pool take write batches only, the others read batches
  // only (host/pg.c, pg_pick): both kinds need a slot, so --pool 1 takes no --write-slots and
  // its one connection carries both, as every connection did before S15 (--write-slots 0).
  if (has_ws && c.pool == 1) {
    return "--pool 1 has no write slot: 2 at least";
  }
  if (has_ws && c.write_slots >= c.pool) {
    return "--write-slots: 0 (no reservation) to --pool - 1";
  }
  if (!has_ws) {
    c.write_slots = c.pool < 2 ? 0 : c.pool / 4 > 1 ? c.pool / 4 : 1;
  }
  // The read batch flushes at --read-batch-max statements, the write batch at --batch-max (S19):
  // a read round stays small while writes keep their commit grouping (host/pg.c, pg_cap).
  if (has_rb && c.read_batch_max > c.batch_max) {
    return "--read-batch-max: 1 to --batch-max";
  }
  if (!has_rb) {
    c.read_batch_max = c.batch_max;
  }
  // A write batch waits --write-wait for more writes to share its commit; canned replies
  // commit nothing, so --db canned keeps one clock for both batches unless told otherwise.
  if (!has_ww) {
    c.write_wait_us = c.db == DB_CANNED ? c.batch_wait_us : 5000;
  }
  if (has_k && c.trace != NULL) {
    c.trace = ep_suffix(c.trace, c.worker);
  }
  if (has_k && c.stats != NULL) {
    c.stats = ep_suffix(c.stats, c.worker);
  }
  host_conf      = c;
  ep.gzip        = gzip;
  ep.accept_read = accept_read;
  return NULL;
}

// The soft descriptor limit, raised to the hard one; the table has a slot for each.
static uint64_t ep_nofile(void) {
  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
    return 1024;
  }
  if (rl.rlim_cur < rl.rlim_max) {
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
    getrlimit(RLIMIT_NOFILE, &rl);
  }
  return (uint64_t)rl.rlim_cur;
}

static int ep_table(uint64_t nofile) {
  ep.cap       = nofile > EP_CAP ? EP_CAP : (uint32_t)nofile;
  int      fl  = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
  size_t   n   = ep.cap;
  uint32_t idle = host_conf.idle_timeout_s;
  void*    st   = mmap(NULL, n, PROT_READ | PROT_WRITE, fl, -1, 0);
  void*    used = mmap(NULL, n * 4, PROT_READ | PROT_WRITE, fl, -1, 0);
  void*    buf  = mmap(NULL, n * HTTP_HEAD, PROT_READ | PROT_WRITE, fl, -1, 0);
  void*    gen  = mmap(NULL, n * 4, PROT_READ | PROT_WRITE, fl, -1, 0);
  void*    big  = mmap(NULL, n * sizeof(EpBig*), PROT_READ | PROT_WRITE, fl, -1, 0);
  void*    out  = mmap(NULL, n * sizeof(EpOut*), PROT_READ | PROT_WRITE, fl, -1, 0);
  void*    ring = mmap(NULL, n * 4, PROT_READ | PROT_WRITE, fl, -1, 0);
  // the last byte's ms per descriptor (ep_touch): the idle clock and a request's age at its reply
  void*    wlast = mmap(NULL, n * 4, PROT_READ | PROT_WRITE, fl, -1, 0);
  if (st == MAP_FAILED || used == MAP_FAILED || buf == MAP_FAILED || gen == MAP_FAILED
    || big == MAP_FAILED || out == MAP_FAILED || ring == MAP_FAILED || wlast == MAP_FAILED) {
    return -1;
  }
  madvise(buf, n * HTTP_HEAD, MADV_NOHUGEPAGE);
  ep.st    = st;
  ep.used  = used;
  ep.buf   = buf;
  ep.gen   = gen;
  ep.big   = big;
  ep.out   = out;
  ep.ring  = ring;
  ep.wlast = wlast;
  if (idle > 0) {
    // the wheel: a bucket for each of the timeout's seconds and two more (ep_wheel_add)
    void* wnext = mmap(NULL, n * 4, PROT_READ | PROT_WRITE, fl, -1, 0);
    void* whead = calloc(idle + 2, sizeof *ep.whead);
    if (wnext == MAP_FAILED || whead == NULL) {
      return -1;
    }
    ep.wnext = wnext;
    ep.whead = whead;
    ep.wn    = idle + 2;
  }
  return 0;
}

// SO_REUSEPORT only with --workers N, N > 1: a process alone binds as before
// S11, so a stale server on the port makes it fail with EADDRINUSE instead
// of sharing the port's connections silently. Workers of different start.sh
// runs would still share; start.sh refuses a port that already listens.
//
// --sock-opts on (S23): TCP_NODELAY, TCP_DEFER_ACCEPT (EP_DEFER_S) and TCP_THIN_LINEAR_TIMEOUTS
// before listen(), TCP_QUICKACK 0 after it (listen() clears the ACK state). An accepted socket
// inherits all but TCP_DEFER_ACCEPT (tests/host/sockopts_test.mjs reads them back from one), so
// ep_accept sets nothing; with --accept-read on it reads the socket at once (S28).
// TCP_DEFER_ACCEPT keeps a connection in the kernel until its first byte arrives; one that sends
// nothing is accepted when the deferral ends (one SYN-ACK timeout, about 1 s) and its client
// answers the SYN-ACK the kernel sends again, and from then on it is an open connection like any
// other: --idle-timeout closes it. An option the kernel refuses fails the listen, as a bind does.
static int ep_listen(uint32_t port) {
  int lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  int one = 1, zero = 0, defer = EP_DEFER_S;
  struct sockaddr_in at = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
    .sin_addr = { htonl(INADDR_ANY) } };
  bool opts = host_conf.sock_opts;
  if (lfd < 0 || setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0
    || (host_conf.workers > 1 && setsockopt(lfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) < 0)
    || (opts && (setsockopt(lfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) < 0
      || setsockopt(lfd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &defer, sizeof defer) < 0
      || setsockopt(lfd, IPPROTO_TCP, TCP_THIN_LINEAR_TIMEOUTS, &one, sizeof one) < 0))
    || bind(lfd, (struct sockaddr*)&at, sizeof at) < 0 || listen(lfd, 65535) < 0
    || (opts && setsockopt(lfd, IPPROTO_TCP, TCP_QUICKACK, &zero, sizeof zero) < 0)) {
    int code = errno;
    if (lfd >= 0) {
      close(lfd);
    }
    errno = code;
    return -1;
  }
  return lfd;
}

// ---- the stats file (--stats) ----

// One line per whole UTC second, written by the first round that ends past
// it: what is in flight then, and the accepts, closes, arrivals and turns
// since the line before. A round that ends seconds late writes one line for
// the last second it passed. The epoll wait never outlasts the next second.
// The segment columns (S23) are each class's growth since the line before:
// the client sockets that closed in it (an open one's count comes in at its
// close, or at stop), and every database socket, read at the line.
static int ep_stats_open(void) {
  ep_st.f = fopen(host_conf.stats, "w");
  if (ep_st.f == NULL) {
    return -1;
  }
  fputs("unix_s\trequests_in_flight\tread_batches\twrite_batches\tbusy_slots\tcancels_in_flight"
    "\tcancels_waiting\thandles_live\tconns_open\taccepts\tcloses\tarrived\taccept_refused\tidle_closes"
    "\treply_400\treply_411\treply_413\treply_431\tturns\tlate_replies"
    "\tclient_segs_in\tclient_segs_out\tclient_data_in\tclient_data_out"
    "\tpg_segs_in\tpg_segs_out\tpg_data_in\tpg_data_out\tring_newest\tring_len\tring_oldest_ms\n", ep_st.f);
  fflush(ep_st.f);
  struct timespec rt;
  clock_gettime(CLOCK_REALTIME, &rt);
  ep_st.sec  = (uint64_t)rt.tv_sec + 1;
  ep_st.next = io_tick() + (1000000000ull - (uint64_t)rt.tv_nsec);
  return 0;
}

// Each database socket's counts join the class's sum before it closes (host/pg.c, pg_down and
// pg_cancel_drop; under libpq, before S25, a socket libpq closed or replaced by itself left the
// sum), so the sum only grows; the floor stays as a guard: each field keeps at least the last
// line's value.
static void ep_segs_floor(HostSegs* d, const HostSegs* last) {
  d->in       = d->in < last->in ? last->in : d->in;
  d->out      = d->out < last->out ? last->out : d->out;
  d->data_in  = d->data_in < last->data_in ? last->data_in : d->data_in;
  d->data_out = d->data_out < last->data_out ? last->data_out : d->data_out;
}

static void ep_stats_line(uint64_t now) {
  if (ep_st.f == NULL || now < ep_st.next) {
    return;
  }
  uint64_t late = (now - ep_st.next) / 1000000000ull;
  uint64_t sec  = ep_st.sec + late;
  ep_st.sec     = sec + 1;
  ep_st.next += (late + 1) * 1000000000ull;
  PgStats p;
  pg_stats(&p);
  HostSegs c = ep.segs, d;
  pg_segs(&d);
  ep_segs_floor(&d, &ep_st.ps);
  fprintf(ep_st.f, "%llu\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu"
    "\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%u\n",
    (unsigned long long)sec, ep.busy, p.read_batches, p.write_batches, p.busy_slots, p.cancels,
    p.cancels_waiting, p.handles, ep.conns, (unsigned long long)(ep.accepts - ep_st.accepts),
    (unsigned long long)(ep.gone - ep_st.gone), (unsigned long long)(ep.arrived - ep_st.arrived),
    (unsigned long long)(ep.accept_refused - ep_st.accept_refused),
    (unsigned long long)(ep.idle_closes - ep_st.idle_closes),
    (unsigned long long)(ep.refused[0] - ep_st.refused[0]), (unsigned long long)(ep.refused[1] - ep_st.refused[1]),
    (unsigned long long)(ep.refused[2] - ep_st.refused[2]), (unsigned long long)(ep.refused[3] - ep_st.refused[3]),
    (unsigned long long)(ep.turns - ep_st.turns), (unsigned long long)(ep.late - ep_st.late),
    (unsigned long long)(c.in - ep_st.cs.in), (unsigned long long)(c.out - ep_st.cs.out),
    (unsigned long long)(c.data_in - ep_st.cs.data_in), (unsigned long long)(c.data_out - ep_st.cs.data_out),
    (unsigned long long)(d.in - ep_st.ps.in), (unsigned long long)(d.out - ep_st.ps.out),
    (unsigned long long)(d.data_in - ep_st.ps.data_in), (unsigned long long)(d.data_out - ep_st.ps.data_out),
    (unsigned long long)(ep.ring_turns - ep_st.ring_turns), (unsigned long long)(ep.ring_len - ep_st.ring_len),
    ep.ring_age_line);
  fflush(ep_st.f);
  ep_st.accepts        = ep.accepts;
  ep_st.gone           = ep.gone;
  ep_st.arrived        = ep.arrived;
  ep_st.accept_refused = ep.accept_refused;
  ep_st.idle_closes    = ep.idle_closes;
  memcpy(ep_st.refused, ep.refused, sizeof ep.refused);
  ep_st.turns          = ep.turns;
  ep_st.late           = ep.late;
  ep_st.ring_turns     = ep.ring_turns;
  ep_st.ring_len       = ep.ring_len;
  ep.ring_age_line     = 0;
  ep_st.cs             = c;
  ep_st.ps             = d;
}

static void ep_on_signal(int sig) {
  (void)sig;
  ep_stop = 1;
}

Term epoll_open_run(Env e, Term* f, IoWork* w) {
  // The workers of submission/start.sh share one stderr. Unbuffered, glibc
  // writes a line longer than 128 bytes in 128-byte pieces, and two workers'
  // pieces interleave (the db stop line did, in tests/host/workers_test.mjs);
  // line-buffered, each line goes out in one write(2).
  setvbuf(stderr, NULL, _IOLBF, 0);
  if (ep.epfd >= 0) {
    return io_fail(e, EBUSY, NULL);
  }
  const char* bad = ep_flags();
  if (bad != NULL) {
    return io_fail(e, EINVAL, bad);
  }
  // Rule 5: at most 512 connections, and each process may hold a cancel
  // connection beside its pool. Nothing is open yet.
  uint64_t conns = ((uint64_t)host_conf.pool + 1) * host_conf.workers;
  if (conns > 512) {
    fprintf(stderr, "bend-epoll: (--pool %u + 1 cancel) x --workers %u = %llu database connections, over 512;"
      " take --pool %u or less\n", host_conf.pool, host_conf.workers, (unsigned long long)conns,
      512 / host_conf.workers - 1);
    exit(64);
  }
  // The descriptors the process needs at the least: --max (the requests a round lexes),
  // --pool, and 16 for the listeners, the epoll sets, the trace and stats files, the
  // cancel connection and the standard streams. Raising the soft limit to the hard one
  // comes first, so a process started with a low soft limit and a high hard one runs.
  uint64_t nofile = ep_nofile();
  uint64_t least  = (uint64_t)host_conf.max + host_conf.pool + 16;
  if (nofile < least) {
    fprintf(stderr, "bend-epoll: the descriptor limit is %llu, under --max %u + --pool %u + 16 = %llu;"
      " raise it (ulimit -n, LimitNOFILE=) or lower --max or --pool\n", (unsigned long long)nofile,
      host_conf.max, host_conf.pool, (unsigned long long)least);
    exit(64);
  }
  const char* secret = getenv("JWT_SECRET");
  if (secret == NULL || *secret == 0) {
    if (host_conf.db != DB_CANNED) {
      return io_fail(e, EINVAL, "JWT_SECRET is required with --db pg");
    }
    secret = "million-users-challenge";  // SPEC.md:13, the development secret
  }
  http_key_set(secret);
  ep.start = io_tick();
  ep.epfd  = epoll_create1(EPOLL_CLOEXEC);
  ep.dbfd  = epoll_create1(EPOLL_CLOEXEC);
  ep.gz    = ep.gzip == EP_GZ_LIBDEFLATE ? libdeflate_alloc_compressor(1) : NULL;
  gz_init();
  if (ep.epfd < 0 || ep.dbfd < 0 || (ep.gzip == EP_GZ_LIBDEFLATE && ep.gz == NULL) || ep_table(nofile) < 0) {
    return io_fail(e, (uint32_t)errno, "epoll or the descriptor table");
  }
  uint32_t ports[2] = { host_conf.port, host_conf.extra_port };
  for (int i = 0; i < 2; i += 1) {
    if (ports[i] == 0) {
      continue;
    }
    ep.lfd[i] = ep_listen(ports[i]);
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = HOST_TAG_LSN | (uint64_t)i };
    if (ep.lfd[i] < 0 || epoll_ctl(ep.epfd, EPOLL_CTL_ADD, ep.lfd[i], &ev) < 0) {
      return io_fail(e, (uint32_t)errno, i == 0 ? "listen on --port" : "listen on --extra-port");
    }
  }
  if (host_conf.trace != NULL) {
    host_trace = fopen(host_conf.trace, "w");
    if (host_trace == NULL) {
      return io_fail(e, (uint32_t)errno, "open --trace");
    }
    setvbuf(host_trace, NULL, _IOFBF, 1 << 20);
    // The first line names the settings the run had, as the start line does (replay.mjs
    // skips a note).
    const struct { const char* k; uint32_t v; } set[] = {
      { "pool", host_conf.pool }, { "write_slots", host_conf.write_slots },
      { "batch_max", host_conf.batch_max }, { "batch_wait_us", host_conf.batch_wait_us },
      { "write_wait_us", host_conf.write_wait_us }, { "db_timeout_ms", host_conf.db_timeout_ms },
      { "max", host_conf.max }, { "worker", host_conf.worker }, { "workers", host_conf.workers },
      { "sock_opts", host_conf.sock_opts }, { "accept_read", ep.accept_read } };
    host_tl_begin();
    jb_str(&host_tl, host_conf.db == DB_PG ? "\"note\":\"start\",\"db\":\"pg\"" : "\"note\":\"start\",\"db\":\"canned\"");
    for (size_t i = 0; i < sizeof set / sizeof set[0]; i += 1) {
      jb_str(&host_tl, ",\"");
      jb_str(&host_tl, set[i].k);
      jb_str(&host_tl, "\":");
      jb_u64(&host_tl, set[i].v);
    }
    if (host_conf.rows_block) {
      // only with --rows block, so that a trace of --rows each is the trace it was before S33
      jb_str(&host_tl, ",\"rows\":\"block\"");
    }
    host_tl_end();
  }
  if (host_conf.stats != NULL && ep_stats_open() < 0) {
    return io_fail(e, (uint32_t)errno, "open --stats");
  }
  struct sigaction sa = { .sa_handler = ep_on_signal };
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  io_sync();
  fprintf(stderr, "bend-epoll: worker %u of %u port %u extra-port %u db %s pool %u batch-max %u"
    " batch-wait %u us write-slots %u write-wait %u us db-timeout %u ms read-batch-max %u sync-max %u"
    " window-us %u us max %u max-head %u max-body %u idle-timeout %u s ring %s rows %s gzip %s sock-opts %s accept-read %s"
    " table %u fds%s%s%s%s\n",
    host_conf.worker, host_conf.workers, host_conf.port, host_conf.extra_port,
    host_conf.db == DB_PG ? "pg" : "canned", host_conf.pool, host_conf.batch_max, host_conf.batch_wait_us,
    host_conf.write_slots, host_conf.write_wait_us,
    host_conf.db_timeout_ms, host_conf.read_batch_max, host_conf.sync_max, host_conf.window_us, host_conf.max,
    host_conf.max_head,
    host_conf.max_body, host_conf.idle_timeout_s, host_conf.ring_lifo ? "lifo" : "fifo",
    host_conf.rows_block ? "block" : "each",
    ep_gzip_name[ep.gzip], host_conf.sock_opts ? "on" : "off", ep.accept_read ? "on" : "off",
    ep.cap, host_conf.trace ? " trace " : "", host_conf.trace ? host_conf.trace : "",
    host_conf.stats ? " stats " : "", host_conf.stats ? host_conf.stats : "");
  return io_done(e, io_tup(e, io_hand(ep.epfd), (Term)host_conf.fuel));
}

// ---- Epoll.turn ----

// Bend can pass only the handle Epoll.open answered. Any other is a bug on
// the Bend side, and the reactor stops rather than act on it.
static void ep_own(Term h, const char* who) {
  int fd = (int)io_hand_v(h);
  if (ep.epfd < 0 || fd != ep.epfd) {
    fprintf(stderr, "bend-epoll: %s got handle %d, not the reactor's %d\n", who, fd, ep.epfd);
    exit(70);
  }
}

// SIGTERM or SIGINT: the turn looks at ep_stop after it ran the last round's
// commands, so the trace is complete when it is flushed here. The client
// sockets still open add their segment counts first; then the stats file gets
// a line for the second in progress, so its accepts, closes, arrivals and
// segments add up to the stop lines' counts.
static void ep_exit(void) {
  if (host_trace != NULL) {
    fflush(host_trace);
  }
  for (uint32_t fd = 0; fd < ep.cap; fd += 1) {
    if (ep.st[fd] & EP_OPEN) {
      host_segs_add(&ep.segs, (int)fd);
    }
  }
  HostSegs d;
  if (ep_st.f != NULL) {
    uint64_t now = io_tick();
    ep_stats_line(now > ep_st.next ? now : ep_st.next);
    fflush(ep_st.f);
    d = ep_st.ps;
  } else {
    pg_segs(&d);
  }
  io_sync();
  fprintf(stderr, "bend-epoll: stopped: %llu requests arrived, %llu replies written, %llu dropped,"
    " %llu connections gone\n", (unsigned long long)ep.arrived, (unsigned long long)ep.sent,
    (unsigned long long)ep.dropped, (unsigned long long)ep.gone);
  fprintf(stderr, "bend-epoll: host: replies 400 %llu, 411 %llu, 413 %llu, 431 %llu; %llu idle closes;"
    " %llu accepts refused (EMFILE or ENFILE)\n", (unsigned long long)ep.refused[0],
    (unsigned long long)ep.refused[1], (unsigned long long)ep.refused[2], (unsigned long long)ep.refused[3],
    (unsigned long long)ep.idle_closes, (unsigned long long)ep.accept_refused);
  fprintf(stderr, "bend-epoll: segments: clients in %llu out %llu data in %llu data out %llu;"
    " postgres in %llu out %llu data in %llu data out %llu; late replies %llu (%u ms or more)\n",
    (unsigned long long)ep.segs.in, (unsigned long long)ep.segs.out, (unsigned long long)ep.segs.data_in,
    (unsigned long long)ep.segs.data_out, (unsigned long long)d.in, (unsigned long long)d.out,
    (unsigned long long)d.data_in, (unsigned long long)d.data_out, (unsigned long long)ep.late,
    (unsigned)EP_LATE_MS);
  if (host_conf.ring_lifo) {
    fprintf(stderr, "bend-epoll: ring: lifo at %u ms, 1 in %u from the old end: %llu of %llu turns lexed newest first, the ring %.1f long at them,"
      " %llu arrivals taken from the old end; the oldest entry at a lex was %u ms old\n", host_conf.ring_age_ms, host_conf.ring_old,
      (unsigned long long)ep.ring_turns, (unsigned long long)ep.turns,
      ep.ring_turns ? (double)ep.ring_len / (double)ep.ring_turns : 0.0, (unsigned long long)ep.ring_old,
      ep.ring_age_max);
  }
  pg_report();
  exit(0);
}

// The longest this turn may wait, in ns from now: the next database deadline (pg_next), at most
// EP_WAIT_NS, and no later than the next stats line, the end of the accept backoff and, with
// --idle-timeout, the next whole second.
static uint64_t ep_wait_ns(uint64_t now) {
  uint64_t next = pg_next(now);
  uint64_t wait = next <= now ? 0 : next - now;
  wait          = wait > EP_WAIT_NS ? EP_WAIT_NS : wait;
  if (ep_st.f != NULL) {
    wait = ep_st.next <= now ? 0 : ep_st.next - now < wait ? ep_st.next - now : wait;
  }
  if (ep.accept_off != 0) {
    uint64_t left = ep.accept_off <= now ? 0 : ep.accept_off - now;
    wait          = left < wait ? left : wait;
  }
  if (ep.whead != NULL) {
    uint64_t left = ep.start + (ep.now_ms / 1000 + 1) * 1000000000ull - now;  // the next whole second
    wait          = left < wait ? left : wait;
  }
  return wait;
}

// --window-us: the comparator's batching window (c-epoll-unorig-username/README.md:45-50). When the
// last epoll_pwait2 brought events and did not fill its array, this turn flushes the read batch
// (the database answers it meanwhile) and sleeps until that wake plus the window, or until the
// first deadline ep_wait_ns names if that comes sooner; its epoll_pwait2 then takes what arrived
// with a zero wait. Not after a wake that brought nothing (an idle server would poll every
// window), nor after a full one (more may be ready: the cap on throughput stays where it is), nor
// in a turn that started with answers --sync-max held over (held): the budget delays them by
// rounds, and the window adds no clock to it. After the flush the turn does not sleep while
// requests on the ring fit the room, idle nodes are due, or a canned batch waits for the next
// turn (pg_pending), as the wait does not block for them. True when the turn took the window.
static bool ep_window(Env e, bool held, uint64_t now) {
  if (host_conf.window_us == 0 || ep.woke == 0 || ep.full || held) {
    return false;
  }
  pg_flush_reads(e, now);
  uint32_t room = pg_room();
  bool     lex  = (room < host_conf.max ? room : host_conf.max) > 0 && ep.count > 0;
  if (!lex && !ep_idle_pending() && !pg_pending()) {
    uint64_t until = ep.wake + (uint64_t)host_conf.window_us * 1000ull;
    uint64_t due   = now + ep_wait_ns(now);
    until          = due < until ? due : until;
    if (until > io_tick()) {
      struct timespec at = { (time_t)(until / 1000000000ull), (long)(until % 1000000000ull) };
      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &at, NULL);  // EINTR: the next round looks at ep_stop
    }
  }
  return true;
}

Term epoll_turn_run(Env e, Term* f, IoWork* w) {
  ep_own(f[0], "Epoll.turn");
  // The last round's commands first, in the order the round answered them.
  // Every event of that round was in hand before its fold began, and the
  // host makes no event between two of them, so running them here is running
  // them at the end of that round (docs/specs/S24-the-pure-round.md).
  ep_cmds(e, f[1], "Epoll.turn");
  if (ep_stop) {
    ep_exit();
  }
  // Every command of the last round's events has run, so no Send can name
  // its rows any more.
  pg_release();
  uint64_t now = io_tick();
  ep_clock(now);
  bool held = pg_pending();  // answers --sync-max held over: pg_tick takes them first
  pg_tick(e, now);
  uint32_t room  = pg_room();
  uint32_t limit = room < host_conf.max ? room : host_conf.max;
  struct timespec ts = { 0, 0 };
  // A slot that --sync-max stopped holds answers in its receive buffer that no socket will report
  // again: the next turn takes them (host/pg.c, pg_resume), so this one does not wait.
  if (ep_window(e, held, now)) {
    room  = pg_room();
    limit = room < host_conf.max ? room : host_conf.max;
  } else if (ev_list_n == 0 && !(limit > 0 && ep.count > 0) && !ep_idle_pending() && !pg_pending()) {
    uint64_t wait = ep_wait_ns(now);
    ts.tv_sec     = (time_t)(wait / 1000000000ull);
    ts.tv_nsec    = (long)(wait % 1000000000ull);
  }
  // --ring lifo: the wait takes every ready descriptor (EP_MAX at most), not --max of them. The
  // kernel hands ready descriptors out oldest first, so a request the turn has not read waits
  // behind every one that became ready before it, and the ring can put first only what was
  // read. --max still bounds what a turn lexes. A consequence: with EP_MAX places almost no
  // wake is full (ep.full below), so ep_window runs after every wake that brought events, where
  // with --max places a wake that returned --max events skipped it.
  int cap = limit > 0 && !host_conf.ring_lifo ? (int)host_conf.max : EP_MAX;
  int n   = epoll_pwait2(limit > 0 ? ep.epfd : ep.dbfd, ep.evs, cap, &ts, NULL);
  if (n < 0) {
    n = 0;  // EINTR: a signal; the next round looks at ep_stop
  }
  now = io_tick();
  ep_clock(now);
  ep.turns += 1;
  ep.wake = now;
  ep.woke = (uint32_t)n;
  ep.full = n == cap;
  for (int i = 0; i < n; i += 1) {
    uint64_t d = ep.evs[i].data.u64;
    if (d & HOST_TAG_PG) {
      pg_ready(e, d, ep.evs[i].events, now);
    } else if (d & HOST_TAG_LSN) {
      ep_accept(e, ep.lfd[d & 1]);
    } else {
      ep_event(e, (uint32_t)d, ep.evs[i].events);
    }
  }
  if (ep.accept_off != 0 && now >= ep.accept_off) {
    ep.accept_off = 0;
    ep_listeners(true);
  }
  ep_idle_run(e);
  room  = pg_room();
  limit = room < host_conf.max ? room : host_conf.max;
  ep_lex_ring(e, limit);
  if (host_trace != NULL) {
    fflush(host_trace);
  }
  ep_stats_line(io_tick());
  Term xs = term_pak(CID(Nil), 0);
  for (uint32_t i = ev_list_n; i > 0; i -= 1) {
    xs = io_node(e, CID(Con), ev_list[i - 1], xs);
  }
  ev_list_n = 0;
  return io_tup(e, f[0], xs);
}

// ---- Epoll.close ----

Term epoll_close_run(Env e, Term* f, IoWork* w) {
  ep_own(f[0], "Epoll.close");
  ep_cmds(e, f[1], "Epoll.close");
  for (uint32_t fd = 0; fd < ep.cap; fd += 1) {
    if (ep.st[fd] & EP_OPEN) {
      close((int)fd);
      free(ep.big[fd]);
      free(ep.out[fd]);
      ep.big[fd] = NULL;
      ep.out[fd] = NULL;
      ep.st[fd]  = 0;
    }
  }
  ep.count = 0;
  pg_close();
  for (int i = 0; i < 2; i += 1) {
    if (ep.lfd[i] >= 0) {
      close(ep.lfd[i]);
      ep.lfd[i] = -1;
    }
  }
  if (host_trace != NULL) {
    fclose(host_trace);
    host_trace = NULL;
  }
  close(ep.dbfd);
  close(ep.epfd);
  ep.dbfd = -1;
  ep.epfd = -1;
  return term_pak(CID(Unit), 0);
}

static void __attribute__((constructor)) epoll_use(void) {
  io_eff(CID(Epoll.open), epoll_open_run);
  io_eff(CID(Epoll.turn), epoll_turn_run);
  io_eff(CID(Epoll.close), epoll_close_run);
}
