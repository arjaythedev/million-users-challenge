/* host: a tiny HTTP/1.1 front for the TeX handler (see PLAN Contract 1).
 * One event loop (epoll on Linux, poll() elsewhere), two listening ports, keep-alive connections. Each complete
 * request is handed to one of WORKERS long-lived `pdftex -fmt=feed '\feedloop'` processes (the rest queue); a worker
 * handles one request at a time and is replaced after WORKER_MAX_REQUESTS of them.
 * The host knows no application logic: it frames HTTP, feeds the workers, and relays resp.txt.
 * It also owns the database: a pool of persistent libpq connections driven from the same loop. TeX talks to it
 * through two FIFOs in the worker's directory: it writes one command line to db.req and reads the answer from db.resp.
 *
 * Worker protocol (everything lives in <workdir>/w<N>/, which is also the worker's cwd):
 *   job.fifo   host -> TeX   one line per request: the path prefix of the request files ("" here, cwd is the dir)
 *   done.fifo  TeX -> host   one line "ok" when resp.txt is complete (TeX opens, writes and closes it each time)
 *   meta.txt req.txt body.txt  written by the host before the job line; newbody.txt resp.txt written by TeX;
 *   db.req db.resp             fresh FIFOs per request, as before
 * Closing job.fifo (EOF) makes the worker exit. */
#define _GNU_SOURCE /* memmem, strcasestr on glibc */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#ifdef __linux__
#include <sys/epoll.h>
#else
#include <poll.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>
#include <libpq-fe.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#define MAX_BODY 16384
#define MAX_HEAD 16384
#define DEFAULT_RECYCLE 5000
#define IDLE_SECS 120
#define CHILD_SECS 30        /* a request that takes longer kills its worker */
#define QUEUE_SECS 60        /* a request that waits longer for a worker gets a 500 */
#define DB_QUEUE_SECS 10
#define DB_CONNECT_MS 5000
#define DB_QUERY_MS 45000
#define DBLINE 1024
#define BODY500 "{\"error\":\"internal server error\"}"

typedef struct { int kind; void *obj; } evsrc;
enum { EV_IN = 1, EV_OUT = 2, EV_ERR = 4 };
typedef struct { evsrc *src; int ev; } evout;

enum state { READING, QUEUED, RUNNING, WRITING };
enum { K_SELF, K_LISTEN, K_CLIENT, K_REQ, K_RESP, K_DB, K_DONE };
enum dbst { DB_NONE, DB_WAITLINE, DB_QUEUED, DB_INFLIGHT, DB_RESP, DB_DONE };
enum wst { W_DOWN, W_IDLE, W_BUSY, W_RETIRING };

struct dbc;

/* A client connection. Small on purpose: tens of thousands of idle keep-alive connections each cost this struct
   and their socket, nothing else (the input buffer is freed whenever it is empty). */
typedef struct conn {
  evsrc src;                       /* event loop tag (kind K_CLIENT) */
  int fd, evm;                     /* socket, interest mask currently registered */
  size_t idx;                      /* position in conns[] */
  struct conn *qnext;              /* run queue link */
  enum state st;
  int close_after, dead;
  char *in; size_t inlen, incap;   /* unparsed input */
  size_t reqlen, hdrlen, clen;     /* current complete request: total bytes, header bytes (incl. CRLFCRLF), body bytes */
  char *out; size_t outlen, outpos;
  time_t last;                     /* queued time while QUEUED, else last activity */
} conn;

/* A TeX worker process and everything about the request it is serving. */
typedef struct worker {
  int id; enum wst st;
  pid_t pid; char *dir;
  int jobfd, jobkeep, donefd;      /* job.fifo writer + a keeper reader (so TeX's open never blocks), done.fifo O_RDWR */
  char dnbuf[16]; size_t dnlen;    /* done.fifo bytes read so far */
  int64_t spawned_ms, retry_at;
  time_t started;                  /* start of the current request (or of the retirement) */
  int killed; unsigned long served;
  conn *cur;
  /* database side of the current request */
  evsrc dsrc[3];                   /* tags for db.req, db.resp and done.fifo */
  enum dbst dbst; int dwait;       /* dwait: db.resp registered for EV_OUT */
  int reqfd, respfd, keepfd;       /* db.req (O_RDWR), db.resp writer, db.resp keeper reader */
  char *dbline; size_t dblen;
  time_t dbqt; struct worker *dbnext;
  struct dbc *dbc;
  char *dout; size_t dlen, dpos;
} worker;

static conn **conns; static size_t nconns, capconns;
static conn **grave; static size_t ngrave, capgrave;   /* closed conns, freed at the end of the loop round */
static conn *runq_head, *runq_tail;                    /* QUEUED conns, oldest first */
static worker *workers; static int nworkers;
static worker **idle; static int nidle;                /* idle workers, most recently used on top */
static worker *dbq_head, *dbq_tail;                    /* workers with a database command waiting */
static int selfpipe[2];
static volatile sig_atomic_t stopping;
static int keep_logs;                                  /* KEEP_WORKDIR=1: keep the TeX logs for debugging */
static unsigned long logseq;
static long recycle_after = DEFAULT_RECYCLE;
static time_t start_time;
static char tmpbase[PATH_MAX], workdir[PATH_MAX + 32], fmtpath[PATH_MAX + 16];

static void die(const char *m) { perror(m); exit(1); }

static void wake(void) { int e = errno; char c = 0; if (write(selfpipe[1], &c, 1) < 0) {} errno = e; }
static void on_chld(int s) { (void)s; wake(); }
static void on_term(int s) { (void)s; stopping = 1; wake(); }

static void set_nb_cloexec(int fd) {
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  fcntl(fd, F_SETFD, FD_CLOEXEC);
}


/* ---------- event loop backend: epoll on Linux, poll() elsewhere ---------- */
/* ev_add/ev_mod/ev_del register an fd with an interest mask (EV_IN | EV_OUT) and an evsrc tag; ev_wait returns the
   ready tags. Level triggered. EV_ERR is reported for hangups and errors whatever the mask. */

#define EVMAX 512

#ifdef __linux__
static int epfd;
static void ev_init(void) { epfd = epoll_create1(EPOLL_CLOEXEC); if (epfd < 0) die("epoll_create1"); }
static int ev_ctl(int op, int fd, int ev, evsrc *s) {
  struct epoll_event e; memset(&e, 0, sizeof e);
  e.events = (ev & EV_IN ? EPOLLIN : 0) | (ev & EV_OUT ? EPOLLOUT : 0); e.data.ptr = s;
  return epoll_ctl(epfd, op, fd, &e);
}
static int ev_add(int fd, int ev, evsrc *s) { return ev_ctl(EPOLL_CTL_ADD, fd, ev, s); }
static int ev_mod(int fd, int ev, evsrc *s) { return ev_ctl(EPOLL_CTL_MOD, fd, ev, s); }
static void ev_del(int fd) { epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL); }
static int ev_wait(evout *out, int ms) {
  struct epoll_event es[EVMAX];
  int n = epoll_wait(epfd, es, EVMAX, ms);
  if (n < 0) { if (errno != EINTR) die("epoll_wait"); return 0; }
  for (int i = 0; i < n; i++) {
    out[i].src = es[i].data.ptr; out[i].ev = 0;
    if (es[i].events & EPOLLIN) out[i].ev |= EV_IN;
    if (es[i].events & EPOLLOUT) out[i].ev |= EV_OUT;
    if (es[i].events & (EPOLLHUP | EPOLLERR)) out[i].ev |= EV_ERR;
  }
  return n;
}
#else
static struct pollfd *pf; static evsrc **psrc; static int *pidx; static size_t pn, pcap, pidxcap;
static void ev_init(void) {}
static int ev_add(int fd, int ev, evsrc *s) {
  if ((size_t)fd >= pidxcap) {
    size_t nc = (size_t)fd * 2 + 64; pidx = realloc(pidx, nc * sizeof *pidx);
    for (size_t i = pidxcap; i < nc; i++) pidx[i] = -1;
    pidxcap = nc;
  }
  if (pidx[fd] >= 0) return -1;
  if (pn == pcap) { pcap = pcap ? pcap * 2 : 256; pf = realloc(pf, pcap * sizeof *pf); psrc = realloc(psrc, pcap * sizeof *psrc); }
  pf[pn].fd = fd; pf[pn].events = (ev & EV_IN ? POLLIN : 0) | (ev & EV_OUT ? POLLOUT : 0); pf[pn].revents = 0;
  psrc[pn] = s; pidx[fd] = (int)pn++;
  return 0;
}
static int ev_mod(int fd, int ev, evsrc *s) {
  if ((size_t)fd >= pidxcap || pidx[fd] < 0) return -1;
  int i = pidx[fd]; pf[i].events = (ev & EV_IN ? POLLIN : 0) | (ev & EV_OUT ? POLLOUT : 0); psrc[i] = s;
  return 0;
}
static void ev_del(int fd) {
  if (fd < 0 || (size_t)fd >= pidxcap || pidx[fd] < 0) return;
  int i = pidx[fd]; pidx[fd] = -1; pn--;
  if ((size_t)i != pn) { pf[i] = pf[pn]; psrc[i] = psrc[pn]; pidx[pf[i].fd] = i; }
}
static int ev_wait(evout *out, int ms) {
  int r = poll(pf, pn, ms), n = 0;
  if (r < 0) { if (errno != EINTR) die("poll"); return 0; }
  for (size_t i = 0; i < pn && n < EVMAX && r > 0; i++) {
    short re = pf[i].revents; if (!re) continue;
    r--; out[n].src = psrc[i]; out[n].ev = 0;
    if (re & POLLIN) out[n].ev |= EV_IN;
    if (re & POLLOUT) out[n].ev |= EV_OUT;
    if (re & (POLLHUP | POLLERR | POLLNVAL)) out[n].ev |= EV_ERR;
    n++;
  }
  return n;
}
#endif
/* Unregisters before closing: a forked child may still hold a dup of the fd for a moment, which would keep an epoll
   registration (and its stale tag) alive after close(). */
static void ev_close(int fd) { if (fd >= 0) { ev_del(fd); close(fd); } }

/* ---------- paths ---------- */

static void find_paths(const char *argv0) {
  char exe[PATH_MAX] = "";
#ifdef __APPLE__
  uint32_t n = sizeof exe; char tmp[PATH_MAX];
  if (_NSGetExecutablePath(tmp, &n) == 0) realpath(tmp, exe);
#else
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  exe[n > 0 ? n : 0] = 0;
#endif
  if (!exe[0] && !realpath(argv0, exe)) die("cannot locate executable");
  char *slash = strrchr(exe, '/'); if (slash) *slash = 0;
  snprintf(fmtpath, sizeof fmtpath, "%s/feed", exe);
  const char *t = getenv("TMPDIR");
  if (t && *t) snprintf(tmpbase, sizeof tmpbase, "%s", t);
  else if (access("/dev/shm", W_OK) == 0) strcpy(tmpbase, "/dev/shm");
  else strcpy(tmpbase, "/tmp");
  size_t l = strlen(tmpbase); while (l > 1 && tmpbase[l - 1] == '/') tmpbase[--l] = 0;
}

/* ---------- connections ---------- */

/* Sets the interest mask of the client socket from its state: READING listens, WRITING waits for room, the rest
   (QUEUED, RUNNING) only watches for errors, so a pipelined request is not read before the current one is answered. */
static void conn_set(conn *c, enum state st) {
  c->st = st;
  if (c->fd < 0) return;
  int want = st == READING ? EV_IN : st == WRITING ? EV_OUT : 0;
  if (want != c->evm) { ev_mod(c->fd, want, &c->src); c->evm = want; }
}

static void conn_free(conn *c) { free(c->in); free(c->out); free(c); }

/* Closes the socket and forgets the conn. If the run queue or a child still points at it, whoever does
   (dispatch / finish) frees it; otherwise it is freed at the end of this loop round (events of this round may
   still carry its tag). */
static void close_conn(conn *c) {
  if (c->dead) return;
  ev_close(c->fd); c->fd = -1; c->dead = 1;
  conns[c->idx] = conns[--nconns]; conns[c->idx]->idx = c->idx;
  if (c->st == QUEUED || c->st == RUNNING) return;   /* the run queue / the worker frees it */
  if (ngrave == capgrave) grave = realloc(grave, (capgrave = capgrave ? capgrave * 2 : 64) * sizeof *grave);
  grave[ngrave++] = c;
}

static void empty_grave(void) { while (ngrave) conn_free(grave[--ngrave]); }

static void accept_on(int lfd) {
  for (int i = 0; i < 1024; i++) {
    int fd = accept(lfd, NULL, NULL);
    if (fd < 0) return; /* EAGAIN, or transient (EMFILE...): try again next round */
    set_nb_cloexec(fd);
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    conn *c = calloc(1, sizeof *c);
    c->src.kind = K_CLIENT; c->src.obj = c;
    c->fd = fd; c->st = READING; c->evm = EV_IN; c->last = time(NULL);
    if (ev_add(fd, EV_IN, &c->src) < 0) { close(fd); free(c); continue; }
    if (nconns == capconns) conns = realloc(conns, (capconns = capconns ? capconns * 2 : 256) * sizeof *conns);
    c->idx = nconns; conns[nconns++] = c;
  }
}

static void try_requests(conn *c);

static const char *reason(int code) {
  switch (code) {
    case 200: return "OK"; case 201: return "Created"; case 400: return "Bad Request";
    case 401: return "Unauthorized"; case 404: return "Not Found"; case 411: return "Length Required";
    case 413: return "Payload Too Large"; case 431: return "Request Header Fields Too Large";
    case 503: return "Service Unavailable";
    default: return code >= 500 ? "Internal Server Error" : code >= 400 ? "Bad Request" : "OK";
  }
}

/* Writes as much of c->out as the socket takes. Returns 1 when everything is sent (conn is then ready for the next request). */
static void flush_out(conn *c) {
  while (c->outpos < c->outlen) {
    ssize_t n = send(c->fd, c->out + c->outpos, c->outlen - c->outpos, 0);
    if (n > 0) c->outpos += n;
    else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    else if (n < 0 && errno == EINTR) continue;
    else { close_conn(c); return; }
  }
  free(c->out); c->out = NULL;
  if (c->close_after) { close_conn(c); return; }
  /* drop the request we just answered, keep pipelined bytes */
  memmove(c->in, c->in + c->reqlen, c->inlen - c->reqlen);
  c->inlen -= c->reqlen; c->reqlen = 0;
  if (!c->inlen) { free(c->in); c->in = NULL; c->incap = 0; } /* an idle keep-alive connection holds no buffer */
  conn_set(c, READING); c->last = time(NULL);
  try_requests(c);
}

static void respond(conn *c, int code, const char *body, size_t blen) {
  char head[256];
  int hl = snprintf(head, sizeof head,
    "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: %s\r\n\r\n",
    code, reason(code), blen, c->close_after ? "close" : "keep-alive");
  c->out = malloc(hl + blen); memcpy(c->out, head, hl); memcpy(c->out + hl, body, blen);
  c->outlen = hl + blen; c->outpos = 0; c->last = time(NULL);
  conn_set(c, WRITING);
  flush_out(c);
}

static void respond_err(conn *c, int code, const char *msg) {
  char b[128]; int n = snprintf(b, sizeof b, "{\"error\":\"%s\"}", msg);
  c->close_after = 1; respond(c, code, b, n);
}

/* Looks at the buffered bytes; if one full request is present, marks the conn QUEUED for the dispatcher. */
static void try_requests(conn *c) {
  if (c->st != READING || c->dead || c->inlen == 0) return;
  char *he = memmem(c->in, c->inlen, "\r\n\r\n", 4);
  if (!he) {
    if (c->inlen > MAX_HEAD) respond_err(c, 431, "headers too large");
    return;
  }
  size_t hdrlen = he - c->in + 4, clen = 0; int ka = 1, have_te = 0;
  if (hdrlen > MAX_HEAD) { respond_err(c, 431, "headers too large"); return; }
  char *p = memchr(c->in, '\n', hdrlen);               /* skip request line */
  if (!p) { respond_err(c, 400, "bad request"); return; }
  if (memmem(c->in, p - c->in, "HTTP/1.0", 8)) ka = 0;
  for (p++; p < c->in + hdrlen - 2;) {
    char *eol = memchr(p, '\n', c->in + hdrlen - p);
    if (!eol) break;
    if (!strncasecmp(p, "content-length:", 15)) clen = strtoul(p + 15, NULL, 10);
    else if (!strncasecmp(p, "transfer-encoding:", 18)) have_te = 1;
    else if (!strncasecmp(p, "connection:", 11)) {
      char line[128]; size_t l = eol - p < 127 ? eol - p : 127; memcpy(line, p, l); line[l] = 0;
      if (strcasestr(line, "close")) ka = 0; else if (strcasestr(line, "keep-alive")) ka = 1;
    }
    p = eol + 1;
  }
  if (have_te) { respond_err(c, 411, "chunked bodies are not supported"); return; }
  if (clen > MAX_BODY) { respond_err(c, 413, "body too large"); return; }
  if (c->inlen < hdrlen + clen) return;                 /* wait for the rest of the body */
  c->hdrlen = hdrlen; c->clen = clen; c->reqlen = hdrlen + clen; c->close_after = !ka;
  conn_set(c, QUEUED); c->last = time(NULL); c->qnext = NULL;
  if (runq_tail) runq_tail->qnext = c; else runq_head = c;
  runq_tail = c;
}

static void on_readable(conn *c) {
  if (c->inlen + 4096 > c->incap) c->in = realloc(c->in, c->incap = (c->incap ? c->incap * 2 : 4096) + 4096);
  ssize_t n = recv(c->fd, c->in + c->inlen, c->incap - c->inlen, 0);
  if (n > 0) { c->inlen += n; c->last = time(NULL); try_requests(c); }
  else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) close_conn(c);
}

/* ---------- database pool ---------- */

#define POST_SELECT \
  "SELECT p.id, p.body, p.created_at, u.username,\n" \
  "       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count\n" \
  "  FROM posts p JOIN users u ON u.id = p.user_id"

enum { S_HEALTH, S_FEED, S_POST, S_CREATE, S_LIKE, NSTMT };
static const char *stmt_sql[NSTMT] = {
  "SELECT 1",
  POST_SELECT "\n ORDER BY p.created_at DESC, p.id DESC LIMIT 20",
  POST_SELECT "\n WHERE p.id = $1",
  "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at",
  "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING",
};

enum dstate { D_DOWN, D_CONNECTING, D_PREPARING, D_IDLE, D_BUSY };
typedef struct dbc {
  PGconn *pg; enum dstate st;
  evsrc src; int evfd, evm;        /* registered socket and interest mask */
  int wantdir; int wantout;        /* while connecting: EV_IN or EV_OUT; when connected: unflushed output */
  int prep, stmt;
  int64_t retry_at, since, backoff;
  worker *owner; PGresult *res;
} dbc;

static dbc *pool; static int npool;
static const char *dburl;
static char last_dberr[256] = "database not connected";

static int64_t now_ms(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

typedef struct { char *p; size_t n, cap; } sbuf;
static void sb_put(sbuf *b, const char *s, size_t n) {
  if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = realloc(b->p, b->cap); }
  memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void sb_str(sbuf *b, const char *s) { sb_put(b, s, strlen(s)); }
static void sb_hex(sbuf *b, const char *s, size_t n) {
  static const char hx[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { char t[2] = { hx[(unsigned char)s[i] >> 4], hx[(unsigned char)s[i] & 15] }; sb_put(b, t, 2); }
}
static void sb_err(sbuf *b, const char *state, const char *msg) {
  sb_str(b, "ERR|"); sb_str(b, state && *state ? state : "CONN"); sb_str(b, "|");
  if (msg) {
    size_t n = strlen(msg); /* libpq messages end with a newline: drop it */
    while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) n--;
    sb_hex(b, msg, n);
  }
  sb_str(b, "\n");
}

static void db_try_write(worker *c);

/* Takes ownership of the buffer and starts pushing it into db.resp. */
static void db_reply(worker *c, sbuf *b) {
  c->dout = b->p; c->dlen = b->n; c->dpos = 0; c->dbst = DB_RESP;
  db_try_write(c);
}
static void db_reply_err(worker *c, const char *state, const char *msg) {
  sbuf b = {0}; sb_err(&b, state, msg); db_reply(c, &b);
}

static void db_try_write(worker *c) {
  if (c->respfd < 0) return;
  while (c->dpos < c->dlen) {
    ssize_t n = write(c->respfd, c->dout + c->dpos, c->dlen - c->dpos);
    if (n > 0) c->dpos += n;
    else if (n < 0 && errno == EINTR) continue;
    else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { /* a feed is bigger than the pipe: wait for room */
      if (!c->dwait) { ev_add(c->respfd, EV_OUT, &c->dsrc[1]); c->dwait = 1; }
      return;
    } else break; /* cannot happen while the keeper fd is open; give up, TeX sees EOF */
  }
  free(c->dout); c->dout = NULL;
  ev_close(c->respfd); c->respfd = -1; /* the only writer: TeX now reads EOF (the keeper keeps unread data alive) */
  c->dbst = DB_DONE;
}

static void db_release(worker *c) {
  if (c->dbc) c->dbc->owner = NULL; /* result is discarded when it arrives */
  c->dbc = NULL;
  if (c->dbst == DB_QUEUED) {                    /* leave the database queue */
    worker **pp = &dbq_head, *prev = NULL;
    while (*pp && *pp != c) { prev = *pp; pp = &(*pp)->dbnext; }
    if (*pp) { *pp = c->dbnext; if (dbq_tail == c) dbq_tail = prev; }
  }
  ev_close(c->reqfd); ev_close(c->respfd); ev_close(c->keepfd);
  c->reqfd = c->respfd = c->keepfd = -1; c->dwait = 0;
  free(c->dout); c->dout = NULL; c->dbst = DB_NONE;
}

static void db_format(worker *c, dbc *d) {
  PGresult *r = d->res; sbuf b = {0};
  ExecStatusType st = r ? PQresultStatus(r) : PGRES_FATAL_ERROR;
  if (!r) sb_err(&b, "CONN", PQerrorMessage(d->pg));
  else if (d->stmt == S_HEALTH && st == PGRES_TUPLES_OK) sb_str(&b, "OK\n");
  else if ((d->stmt == S_FEED || d->stmt == S_POST) && st == PGRES_TUPLES_OK) {
    for (int i = 0; i < PQntuples(r); i++) {
      sb_str(&b, "P|"); sb_str(&b, PQgetvalue(r, i, 0)); sb_str(&b, "|"); sb_str(&b, PQgetvalue(r, i, 2)); sb_str(&b, "|");
      sb_hex(&b, PQgetvalue(r, i, 3), strlen(PQgetvalue(r, i, 3))); sb_str(&b, "|"); sb_str(&b, PQgetvalue(r, i, 4)); sb_str(&b, "|");
      sb_hex(&b, PQgetvalue(r, i, 1), strlen(PQgetvalue(r, i, 1))); sb_str(&b, "\n");
    }
  } else if (d->stmt == S_CREATE && st == PGRES_TUPLES_OK && PQntuples(r) == 1) {
    sb_str(&b, "C|"); sb_str(&b, PQgetvalue(r, 0, 0)); sb_str(&b, "|"); sb_str(&b, PQgetvalue(r, 0, 1)); sb_str(&b, "\n");
  } else if (d->stmt == S_LIKE && st == PGRES_COMMAND_OK) {
    sb_str(&b, strcmp(PQcmdTuples(r), "0") ? "L|1\n" : "L|0\n");
  } else {
    const char *sq = PQresultErrorField(r, PG_DIAG_SQLSTATE);
    sb_err(&b, sq, PQresultErrorMessage(r));
  }
  db_reply(c, &b);
}

static void dbc_close_pg(dbc *d) {
  if (d->evfd >= 0) ev_del(d->evfd);
  d->evfd = -1; d->evm = 0;
  if (d->pg) PQfinish(d->pg);
  d->pg = NULL;
}

/* Makes the registration match what the connection wants now. While connecting libpq may swap the socket (next
   address), so a connecting dbc is re-registered from scratch every time. */
static void dbc_sync(dbc *d) {
  if (!d->pg) return;
  int sfd = PQsocket(d->pg);
  if (sfd < 0) return;
  int want = d->st == D_CONNECTING ? d->wantdir : (EV_IN | (d->wantout ? EV_OUT : 0));
  if (d->st == D_CONNECTING || sfd != d->evfd) {
    if (d->evfd >= 0) ev_del(d->evfd);
    ev_add(sfd, want, &d->src); d->evfd = sfd; d->evm = want;
  } else if (want != d->evm) { ev_mod(sfd, want, &d->src); d->evm = want; }
}

/* The connection is unusable: answer its owner (if still around), reconnect later with backoff. */
static void dbc_fail(dbc *d, const char *why) {
  const char *m = d->pg && PQerrorMessage(d->pg) && *PQerrorMessage(d->pg) ? PQerrorMessage(d->pg) : why;
  snprintf(last_dberr, sizeof last_dberr, "%s", m);
  size_t l = strlen(last_dberr); while (l && (last_dberr[l - 1] == '\n' || last_dberr[l - 1] == '\r')) last_dberr[--l] = 0;
  if (d->res) { PQclear(d->res); d->res = NULL; }
  if (d->owner) { worker *o = d->owner; d->owner = NULL; o->dbc = NULL; db_reply_err(o, "CONN", last_dberr); }
  dbc_close_pg(d);
  d->st = D_DOWN; d->backoff = d->backoff ? (d->backoff * 2 > 2000 ? 2000 : d->backoff * 2) : 100;
  d->retry_at = now_ms() + d->backoff;
}

static void dbc_connect(dbc *d) {
  const char *keys[] = {"dbname", "options", NULL};
  const char *vals[] = {dburl && *dburl ? dburl : NULL, "-c TimeZone=UTC -c DateStyle=ISO", NULL};
  d->pg = PQconnectStartParams(keys, vals, 1);
  d->since = now_ms();
  if (!d->pg || PQstatus(d->pg) == CONNECTION_BAD) { dbc_fail(d, "cannot start connection"); return; }
  d->st = D_CONNECTING; d->wantdir = EV_OUT; d->wantout = 0;
  dbc_sync(d);
}

static int dbc_flush(dbc *d) {
  int f = PQflush(d->pg);
  if (f < 0) { dbc_fail(d, "connection lost"); return -1; }
  d->wantout = f;
  return 0;
}

static void dbc_prepare_next(dbc *d) {
  if (d->prep >= NSTMT) { d->st = D_IDLE; d->backoff = 0; return; }
  char name[16]; snprintf(name, sizeof name, "s%d", d->prep);
  if (!PQsendPrepare(d->pg, name, stmt_sql[d->prep], 0, NULL)) { dbc_fail(d, "prepare failed"); return; }
  d->st = D_PREPARING; dbc_flush(d);
}

/* Starts the command line of c on idle connection d. */
static void dbc_run(dbc *d, worker *c) {
  char line[DBLINE]; memcpy(line, c->dbline, sizeof line);
  char *t[5] = {0}; int nt = 0, bad = 0; char *sp = NULL;
  for (char *w = strtok_r(line, " \t\r\n", &sp); w && nt < 5; w = strtok_r(NULL, " \t\r\n", &sp)) t[nt++] = w;
  const char *vals[2] = {0}; int np = 0, stmt = -1; char *body = NULL;
  if (nt == 1 && !strcmp(t[0], "health")) stmt = S_HEALTH;
  else if (nt == 1 && !strcmp(t[0], "feed")) stmt = S_FEED;
  else if (nt == 2 && !strcmp(t[0], "post")) { stmt = S_POST; vals[np++] = t[1]; }
  else if (nt == 3 && !strcmp(t[0], "like")) { stmt = S_LIKE; vals[np++] = t[1]; vals[np++] = t[2]; }
  else if (nt == 3 && !strcmp(t[0], "create") && !strchr(t[2], '/')) {
    char path[PATH_MAX + 300]; snprintf(path, sizeof path, "%s/%s", c->dir, t[2]);
    FILE *f = fopen(path, "rb");
    if (!f) { db_reply_err(c, "FILE", "cannot read body file"); return; }
    size_t cap = 4096, n = 0, k; body = malloc(cap + 1);
    while ((k = fread(body + n, 1, cap - n, f)) > 0) { n += k; if (n == cap) body = realloc(body, (cap *= 2) + 1); }
    fclose(f);
    if (n && body[n - 1] == '\n') n--; /* strip ONE trailing newline */
    body[n] = 0;
    stmt = S_CREATE; vals[np++] = t[1]; vals[np++] = body;
  } else bad = 1;
  if (bad) { db_reply_err(c, "USAGE", "unknown command"); return; }
  char name[16]; snprintf(name, sizeof name, "s%d", stmt);
  int ok = PQsendQueryPrepared(d->pg, name, np, vals, NULL, NULL, 0);
  free(body);
  d->stmt = stmt; d->st = D_BUSY; d->owner = c; d->since = now_ms(); c->dbc = d; c->dbst = DB_INFLIGHT;
  if (!ok) { dbc_fail(d, "send failed"); return; }
  if (dbc_flush(d) == 0) dbc_sync(d);
}

/* Reads whatever arrived. Handles the PREPARING and BUSY states. */
static void dbc_input(dbc *d) {
  if (!PQconsumeInput(d->pg)) { dbc_fail(d, "connection lost"); return; }
  while (d->pg && !PQisBusy(d->pg)) {
    PGresult *r = PQgetResult(d->pg);
    if (r) { if (d->res) PQclear(r); else d->res = r; continue; }
    if (d->st == D_PREPARING) {
      int good = d->res && PQresultStatus(d->res) == PGRES_COMMAND_OK;
      if (!good) {
        snprintf(last_dberr, sizeof last_dberr, "%s", d->res ? PQresultErrorMessage(d->res) : "prepare failed");
        PQclear(d->res); d->res = NULL; dbc_fail(d, last_dberr); return;
      }
      PQclear(d->res); d->res = NULL; d->prep++; dbc_prepare_next(d); return;
    }
    /* D_BUSY: the query is complete */
    worker *o = d->owner; d->owner = NULL;
    if (o) { o->dbc = NULL; db_format(o, d); }
    if (d->res) { PQclear(d->res); d->res = NULL; }
    if (PQstatus(d->pg) != CONNECTION_OK) { dbc_fail(d, "connection lost"); return; }
    d->st = D_IDLE;
    return;
  }
}

static void dbc_event(dbc *d, int rev) {
  if (d->st == D_CONNECTING) {
    PostgresPollingStatusType ps = PQconnectPoll(d->pg);
    if (ps == PGRES_POLLING_READING) d->wantdir = EV_IN;
    else if (ps == PGRES_POLLING_WRITING) d->wantdir = EV_OUT;
    else if (ps == PGRES_POLLING_OK) {
      if (PQsetnonblocking(d->pg, 1) != 0) { dbc_fail(d, "cannot set nonblocking"); return; }
      d->prep = 0; dbc_prepare_next(d);
    } else { dbc_fail(d, "connection failed"); return; }
    dbc_sync(d);
    return;
  }
  if ((rev & EV_OUT) && d->wantout && dbc_flush(d) < 0) return;
  if (d->st == D_IDLE) {
    if (!PQconsumeInput(d->pg) || PQstatus(d->pg) != CONNECTION_OK) dbc_fail(d, "connection lost");
    else if (rev & EV_ERR) dbc_fail(d, "connection lost");
    else dbc_sync(d);
    return;
  }
  if (rev & (EV_IN | EV_ERR)) dbc_input(d);
  if (d->pg) dbc_sync(d);
}

static void db_init(void) {
  long nc = sysconf(_SC_NPROCESSORS_ONLN); if (nc < 1) nc = 1;
  long n = 2 * nc < 64 ? 2 * nc : 64;
  if (getenv("DB_POOL") && atol(getenv("DB_POOL")) > 0) n = atol(getenv("DB_POOL"));
  if (n > 512) n = 512;
  npool = (int)n; dburl = getenv("DATABASE_URL");
  pool = calloc(npool, sizeof *pool);
  for (int i = 0; i < npool; i++) { pool[i].evfd = -1; pool[i].src = (evsrc){K_DB, &pool[i]}; dbc_connect(&pool[i]); }
}

/* Hands queued commands to idle connections (oldest first); fails them when the pool is down or they waited too long.
   Returns 1 while some pool connection is down (the loop then wakes up often enough to retry). */
static int db_any_down(void) {
  for (int i = 0; i < npool; i++) if (pool[i].st == D_DOWN) return 1;
  return 0;
}

static int db_dispatch(void) {
  int64_t now = now_ms();
  for (int i = 0; i < npool; i++)
    if (pool[i].st == D_DOWN && now >= pool[i].retry_at) dbc_connect(&pool[i]);
  for (int i = 0; i < npool; i++) {
    dbc *d = &pool[i];
    if (d->st == D_CONNECTING && now - d->since > DB_CONNECT_MS) dbc_fail(d, "connection timeout");
    else if (d->st == D_BUSY && now - d->since > DB_QUERY_MS) dbc_fail(d, "query timeout");
  }
  int live = 0, down = 0;
  for (int i = 0; i < npool; i++) { if (pool[i].st != D_DOWN) live++; else down = 1; }
  time_t tnow = time(NULL);
  while (dbq_head) {
    worker *best = dbq_head;
    if (!live || tnow - best->dbqt > DB_QUEUE_SECS) {
      dbq_head = best->dbnext; if (!dbq_head) dbq_tail = NULL;
      db_reply_err(best, "CONN", live ? "timed out waiting for a database connection" : last_dberr);
      continue;
    }
    dbc *idle = NULL;
    for (int i = 0; i < npool; i++) if (pool[i].st == D_IDLE) { idle = &pool[i]; break; }
    if (!idle) break;
    dbq_head = best->dbnext; if (!dbq_head) dbq_tail = NULL;
    dbc_run(idle, best);
  }
  return down;
}

static void db_read_line(worker *c) {
  for (;;) {
    ssize_t n = read(c->reqfd, c->dbline + c->dblen, DBLINE - 1 - c->dblen);
    if (n > 0) {
      c->dblen += n;
      char *nl = memchr(c->dbline, '\n', c->dblen);
      if (nl) {
        *nl = 0; ev_del(c->reqfd); c->dbst = DB_QUEUED; c->dbqt = time(NULL); c->dbnext = NULL;
        if (dbq_tail) dbq_tail->dbnext = c; else dbq_head = c;
        dbq_tail = c;
        return;
      }
      if (c->dblen >= DBLINE - 1) { ev_del(c->reqfd); db_reply_err(c, "USAGE", "command too long"); return; }
    } else if (n < 0 && errno == EINTR) continue;
    else return; /* EAGAIN (or nothing more) */
  }
}

/* ---------- TeX workers ---------- */

static int write_file(const char *dir, const char *name, const char *data, size_t len) {
  char path[PATH_MAX + 16]; snprintf(path, sizeof path, "%s/%s", dir, name);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return -1;
  size_t off = 0;
  while (off < len) { ssize_t n = write(fd, data + off, len - off); if (n <= 0) { close(fd); return -1; } off += n; }
  return close(fd);
}

static void rm_rf(const char *dir) {
  DIR *d = opendir(dir);
  if (d) {
    struct dirent *e; char path[PATH_MAX + 300];
    while ((e = readdir(d))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
      struct stat sb;
      if (lstat(path, &sb) == 0 && S_ISDIR(sb.st_mode)) rm_rf(path); else unlink(path);
    }
    closedir(d);
  }
  rmdir(dir);
}

/* Removes the work directories of hosts that are gone (killed without cleaning up). */
static void rm_stale(void) {
  DIR *d = opendir(tmpbase); if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (strncmp(e->d_name, "texcgi.", 7)) continue;
    char *end; long pid = strtol(e->d_name + 7, &end, 10);
    if (*end || pid <= 0 || (kill((pid_t)pid, 0) == 0 || errno == EPERM)) continue;
    char path[PATH_MAX + 300]; snprintf(path, sizeof path, "%s/%s", tmpbase, e->d_name); rm_rf(path);
  }
  closedir(d);
}

static int64_t now_ms(void);
static void dispatch(void);

static void idle_push(worker *w) { w->st = W_IDLE; idle[nidle++] = w; }
static void idle_remove(worker *w) {
  for (int i = 0; i < nidle; i++) if (idle[i] == w) { idle[i] = idle[--nidle]; return; }
}

static void spawn_worker(worker *w) {
  rm_rf(w->dir);
  char p[PATH_MAX + 32];
  w->jobfd = w->jobkeep = w->donefd = -1;
  int ok = mkdir(w->dir, 0700) == 0;
  if (ok) {
    snprintf(p, sizeof p, "%s/job.fifo", w->dir); ok = mkfifo(p, 0600) == 0;
    if (ok) { /* a keeper reader first: the non-blocking writer open needs a reader, and so does TeX's own open */
      w->jobkeep = open(p, O_RDONLY | O_NONBLOCK);
      w->jobfd = w->jobkeep >= 0 ? open(p, O_WRONLY | O_NONBLOCK) : -1;
      ok = w->jobkeep >= 0 && w->jobfd >= 0;
    }
  }
  if (ok) {
    snprintf(p, sizeof p, "%s/done.fifo", w->dir); ok = mkfifo(p, 0600) == 0;
    if (ok) { w->donefd = open(p, O_RDWR | O_NONBLOCK); ok = w->donefd >= 0; } /* O_RDWR: never sees EOF between answers */
  }
  if (ok) {
    set_nb_cloexec(w->jobkeep); set_nb_cloexec(w->jobfd); set_nb_cloexec(w->donefd);
    ok = ev_add(w->donefd, EV_IN, &w->dsrc[2]) == 0;
  }
  char fmtarg[PATH_MAX + 32]; snprintf(fmtarg, sizeof fmtarg, "-fmt=%s", fmtpath);
  pid_t pid = ok ? fork() : -1;
  if (pid == 0) {
    signal(SIGPIPE, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL);
    int dn = open("/dev/null", O_RDWR);
    if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }
    if (chdir(w->dir) != 0) _exit(127);
    /* no \openout lines in the log, long log lines; files are plain names in the cwd, so the default
       openin_any / openout_any ("paranoid") are fine */
    setenv("log_openout", "0", 1); setenv("max_print_line", "1000000", 1);
    setenv("error_line", "254", 1); setenv("half_error_line", "238", 1);
    execlp("pdftex", "pdftex", fmtarg, "-8bit", "-interaction=batchmode", "-halt-on-error", "-jobname=feedw",
           "\\def\\feedctl{}\\feedloop", (char *)NULL);
    _exit(127);
  }
  if (pid < 0) {                                      /* try again shortly */
    ev_close(w->donefd); close(w->jobfd); close(w->jobkeep); w->jobfd = w->jobkeep = w->donefd = -1;
    w->st = W_DOWN; w->retry_at = now_ms() + 500; w->pid = 0; return;
  }
  w->pid = pid; w->spawned_ms = now_ms(); w->served = 0; w->killed = 0; w->dnlen = 0; w->cur = NULL;
  idle_push(w);
}

/* Tears the worker's files down; the process is gone or about to be. */
static void worker_cleanup(worker *w) {
  idle_remove(w);
  ev_close(w->donefd); ev_close(w->jobfd); ev_close(w->jobkeep);
  w->jobfd = w->jobkeep = w->donefd = -1;
  if (keep_logs) {
    char from[PATH_MAX + 32], to[PATH_MAX + 64];
    snprintf(from, sizeof from, "%s/feedw.log", w->dir); snprintf(to, sizeof to, "%s/w%d.%lu.log", workdir, w->id, ++logseq);
    rename(from, to);
  }
  rm_rf(w->dir);
  w->pid = 0; w->st = W_DOWN;
}

/* Reads <dir>/resp.txt (if the worker said it is done) and answers the client. */
static void finish_request(worker *w, int ok) {
  conn *c = w->cur; w->cur = NULL;
  char path[PATH_MAX + 16], buf[1 << 20]; size_t n = 0;
  snprintf(path, sizeof path, "%s/resp.txt", w->dir);
  FILE *f = ok ? fopen(path, "rb") : NULL;
  if (f) { n = fread(buf, 1, sizeof buf, f); fclose(f); }
  db_release(w);
  static const char *const files[] = {"meta.txt", "req.txt", "body.txt", "newbody.txt", "resp.txt", "db.req", "db.resp"};
  for (size_t i = 0; i < sizeof files / sizeof *files; i++) { snprintf(path, sizeof path, "%s/%s", w->dir, files[i]); unlink(path); }
  if (c->dead) { conn_free(c); return; }
  int code = 0;
  if (n >= 5 && buf[3] == '\n' && buf[0] >= '1' && buf[0] <= '5' && buf[1] >= '0' && buf[1] <= '9' && buf[2] >= '0' && buf[2] <= '9')
    code = (buf[0] - '0') * 100 + (buf[1] - '0') * 10 + (buf[2] - '0');
  if (!code) { respond(c, 500, BODY500, strlen(BODY500)); return; }
  size_t bl = n - 4;
  while (bl && (buf[4 + bl - 1] == '\n' || buf[4 + bl - 1] == '\r')) bl--;
  if (!bl) { respond(c, 500, BODY500, strlen(BODY500)); return; }
  respond(c, code, buf + 4, bl);
}

/* The worker has served enough: closing job.fifo makes TeX leave its loop; the reaper starts the next one. */
static void retire(worker *w) {
  w->st = W_RETIRING; w->started = time(NULL);
  ev_close(w->jobfd); w->jobfd = -1;
}

static void worker_idle(worker *w) {
  if (recycle_after > 0 && w->served >= (unsigned long)recycle_after) retire(w);
  else idle_push(w);
}

/* done.fifo is readable: a complete line means resp.txt is ready. */
static void on_done(worker *w) {
  if (w->donefd < 0) return;
  for (;;) {
    ssize_t n = read(w->donefd, w->dnbuf + w->dnlen, sizeof w->dnbuf - w->dnlen);
    if (n <= 0) return;
    w->dnlen += n;
    char *nl = memchr(w->dnbuf, '\n', w->dnlen);
    if (!nl) { if (w->dnlen == sizeof w->dnbuf) w->dnlen = 0; continue; }
    size_t used = nl - w->dnbuf + 1, rest = w->dnlen - used;
    int good = used == 3 && !memcmp(w->dnbuf, "ok", 2);
    memmove(w->dnbuf, w->dnbuf + used, rest); w->dnlen = rest;
    if (w->st == W_BUSY) { w->served++; finish_request(w, good); worker_idle(w); }
    if (w->st != W_IDLE && w->st != W_RETIRING) return;
  }
}

/* A worker process ended: answer its request (500, unless it had already answered), start a replacement. */
static void worker_exited(worker *w, int status) {
  int expected = w->st == W_RETIRING;
  if (w->st == W_BUSY) {
    on_done(w);                                   /* it may have answered just before dying */
    if (w->st == W_BUSY) { (void)status; w->served++; finish_request(w, 0); }
  }
  int64_t lived = now_ms() - w->spawned_ms;
  worker_cleanup(w);
  if (stopping) return;
  if (expected || lived >= 1000) spawn_worker(w);
  else { w->retry_at = now_ms() + 500; }          /* dying at once (no pdftex?): do not spin */
}

static void start_request(worker *w, conn *c) {
  /* req.txt: request line + headers, CRLF -> LF, no blank line at the end (every line LF-terminated) */
  size_t hl = c->hdrlen - 4, k = 0; char *req = malloc(hl + 2);
  for (size_t i = 0; i < hl; i++) if (!(c->in[i] == '\r' && i + 1 < hl + 2 && c->in[i + 1] == '\n')) req[k++] = c->in[i];
  req[k++] = '\n';
  char meta[512]; const char *secret = getenv("JWT_SECRET");
  int ml = snprintf(meta, sizeof meta, "%ld\n%ld\n%s\n", (long)time(NULL), (long)start_time, secret ? secret : "");
  if (ml >= (int)sizeof meta) ml = sizeof meta - 1;
  int bad = write_file(w->dir, "meta.txt", meta, ml) || write_file(w->dir, "req.txt", req, k)
         || write_file(w->dir, "body.txt", c->in + c->hdrlen, c->clen);
  free(req);
  if (!bad) {
    char p1[PATH_MAX + 16], p2[PATH_MAX + 16];
    snprintf(p1, sizeof p1, "%s/db.req", w->dir); snprintf(p2, sizeof p2, "%s/db.resp", w->dir);
    if (mkfifo(p1, 0600) || mkfifo(p2, 0600)) bad = 1;
    else {
      /* db.req read-write so the host is also a writer (no hangup games); db.resp: a keeper reader first, so the
         non-blocking writer open works and unread data survives until TeX opens it */
      w->reqfd = open(p1, O_RDWR | O_NONBLOCK);
      w->keepfd = open(p2, O_RDONLY | O_NONBLOCK);
      w->respfd = w->keepfd >= 0 ? open(p2, O_WRONLY | O_NONBLOCK) : -1;
      if (w->reqfd < 0 || w->keepfd < 0 || w->respfd < 0) bad = 1;
      else {
        set_nb_cloexec(w->reqfd); set_nb_cloexec(w->keepfd); set_nb_cloexec(w->respfd);
        if (ev_add(w->reqfd, EV_IN, &w->dsrc[0]) < 0) bad = 1;
      }
    }
  }
  if (!bad && write(w->jobfd, "\n", 1) != 1) bad = 1;      /* the job line: an empty path prefix */
  w->cur = c; conn_set(c, RUNNING);
  if (bad) {
    w->st = W_BUSY; w->served++; finish_request(w, 0); worker_idle(w); return;
  }
  w->dbst = DB_WAITLINE; w->dblen = 0; w->dwait = 0;
  w->st = W_BUSY; w->started = time(NULL);
}

static void dispatch(void) {
  while (runq_head && nidle > 0) {
    conn *c = runq_head;
    runq_head = c->qnext; if (!runq_head) runq_tail = NULL;
    if (c->dead) { conn_free(c); continue; }               /* the client left while it was queued */
    if (time(NULL) - c->last > QUEUE_SECS) { conn_set(c, READING); respond_err(c, 500, "internal server error"); continue; }
    start_request(idle[--nidle], c);
  }
}

static void reap(void) {
  char tmp[64];
  while (read(selfpipe[0], tmp, sizeof tmp) > 0) {}
  int status; pid_t pid;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    for (int i = 0; i < nworkers; i++)
      if (workers[i].pid == pid) { worker_exited(&workers[i], status); break; }
}

/* Per-round timers: request time limits, retirements that hang, workers waiting to be restarted. */
static void tick(void) {
  time_t now = time(NULL); int64_t nm = now_ms();
  for (int i = 0; i < nworkers; i++) {
    worker *w = &workers[i];
    if (w->st == W_BUSY && now - w->started > CHILD_SECS && !w->killed) { kill(w->pid, SIGKILL); w->killed = 1; }
    else if (w->st == W_RETIRING && now - w->started > 5 && !w->killed) { kill(w->pid, SIGKILL); w->killed = 1; }
    else if (w->st == W_DOWN && nm >= w->retry_at && !stopping) spawn_worker(w);
  }
}

/* Closing job.fifo lets a worker finish its log; whoever is still there after a second is killed. */
static void shutdown_all(void) {
  for (int i = 0; i < nworkers; i++) if (workers[i].jobfd >= 0) { close(workers[i].jobfd); workers[i].jobfd = -1; }
  for (int t = 0; t < 100; t++) {
    int alive = 0;
    for (int i = 0; i < nworkers; i++) if (workers[i].pid > 0 && waitpid(workers[i].pid, NULL, WNOHANG) == 0) alive = 1;
    if (!alive) break;
    usleep(10000);
  }
  for (int i = 0; i < nworkers; i++) if (workers[i].pid > 0) kill(workers[i].pid, SIGKILL);
  while (waitpid(-1, NULL, 0) > 0) {}
  for (int i = 0; i < nworkers; i++) if (workers[i].pid > 0) worker_cleanup(&workers[i]);
  if (!keep_logs) rm_rf(workdir);
}

/* ---------- main ---------- */

static int listen_on(const char *host, const char *port) {
  struct addrinfo hints = {0}, *ai;
  hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_PASSIVE;
  if (getaddrinfo(host, port, &hints, &ai) != 0) { fprintf(stderr, "bad HOST/PORT %s:%s\n", host, port); exit(1); }
  int fd = socket(ai->ai_family, ai->ai_socktype, 0);
  if (fd < 0) die("socket");
  int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (bind(fd, ai->ai_addr, ai->ai_addrlen) < 0) die("bind");
  if (listen(fd, 4096) < 0) die("listen");
  set_nb_cloexec(fd); freeaddrinfo(ai);
  return fd;
}

int main(int argc, char **argv) {
  (void)argc;
  const char *host = getenv("HOST"), *port = getenv("PORT"), *extra = getenv("EXTRA_PORT");
  if (!host || !*host) host = "0.0.0.0";
  if (!port || !*port) port = "3000";
  find_paths(argv[0]);
  start_time = time(NULL);
  long nc = sysconf(_SC_NPROCESSORS_ONLN); if (nc < 1) nc = 1;
  long nw = 2 * nc;                                   /* TeX workers wait on Postgres half the time: 2 x cores */
  if (getenv("WORKERS") && atol(getenv("WORKERS")) > 0) nw = atol(getenv("WORKERS"));
  else if (getenv("MAX_CHILDREN") && atol(getenv("MAX_CHILDREN")) > 0) nw = atol(getenv("MAX_CHILDREN")); /* old name */
  if (nw > 512) nw = 512;
  nworkers = (int)nw;
  keep_logs = getenv("KEEP_WORKDIR") && *getenv("KEEP_WORKDIR") && *getenv("KEEP_WORKDIR") != '0';
  if (getenv("WORKER_MAX_REQUESTS") && *getenv("WORKER_MAX_REQUESTS")) recycle_after = atol(getenv("WORKER_MAX_REQUESTS"));

  signal(SIGPIPE, SIG_IGN);
  if (pipe(selfpipe) < 0) die("pipe");
  set_nb_cloexec(selfpipe[0]); set_nb_cloexec(selfpipe[1]);
  struct sigaction sa = {0}; sa.sa_handler = on_chld; sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  sigemptyset(&sa.sa_mask); sigaction(SIGCHLD, &sa, NULL);
  sa.sa_handler = on_term; sa.sa_flags = 0; sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);

  int lfd[2], nl = 0;
  lfd[nl++] = listen_on(host, port);
  if (extra && *extra && strcmp(extra, port)) lfd[nl++] = listen_on(host, extra);

  rm_stale();
  snprintf(workdir, sizeof workdir, "%s/texcgi.%ld", tmpbase, (long)getpid());
  if (mkdir(workdir, 0700) != 0) die("mkdir workdir");
  fprintf(stderr, "host: listening on %s:%s%s%s, %d TeX workers (recycled after %ld requests), workdir %s, fmt %s\n", host, port,
          nl > 1 ? " and " : "", nl > 1 ? extra : "", nworkers, recycle_after, workdir, fmtpath);

  ev_init();
  evsrc s_self = {K_SELF, NULL}, s_listen[2] = {{K_LISTEN, NULL}, {K_LISTEN, NULL}};
  if (ev_add(selfpipe[0], EV_IN, &s_self) < 0) die("ev_add");
  for (int i = 0; i < nl; i++) { s_listen[i].obj = (void *)(intptr_t)lfd[i]; if (ev_add(lfd[i], EV_IN, &s_listen[i]) < 0) die("ev_add"); }
  db_init();
  workers = calloc(nworkers, sizeof *workers); idle = calloc(nworkers, sizeof *idle);
  for (int i = 0; i < nworkers; i++) {
    worker *w = &workers[i]; w->id = i;
    w->dsrc[0] = (evsrc){K_REQ, w}; w->dsrc[1] = (evsrc){K_RESP, w}; w->dsrc[2] = (evsrc){K_DONE, w};
    w->reqfd = w->respfd = w->keepfd = -1; w->dbline = malloc(DBLINE);
    size_t dl = strlen(workdir) + 16; w->dir = malloc(dl); snprintf(w->dir, dl, "%s/w%d", workdir, i);
    spawn_worker(w);
  }
  evout evs[EVMAX]; time_t last_sweep = 0;
  while (!stopping) {
    int any_down = db_any_down();
    for (int i = 0; i < nworkers; i++) if (workers[i].st == W_DOWN) any_down = 1;
    int n = ev_wait(evs, any_down ? 50 : 1000);
    for (int i = 0; i < n; i++) {
      evsrc *src = evs[i].src; int rev = evs[i].ev;
      switch (src->kind) {
        case K_SELF: reap(); break;
        case K_LISTEN: accept_on((int)(intptr_t)src->obj); break;
        case K_DB: dbc_event(src->obj, rev); break;
        case K_REQ: { worker *w = src->obj; if (w->st == W_BUSY && w->dbst == DB_WAITLINE) db_read_line(w); break; }
        case K_RESP: { worker *w = src->obj; if (w->st == W_BUSY && w->dbst == DB_RESP) db_try_write(w); break; }
        case K_DONE: on_done(src->obj); break;
        default: {
          conn *c = src->obj;
          if (c->dead) break;
          if (c->st == READING && (rev & (EV_IN | EV_ERR))) on_readable(c);
          else if (c->st == WRITING && (rev & (EV_OUT | EV_ERR))) flush_out(c);
          else if (rev & EV_ERR) close_conn(c);   /* reset while its request is queued or running */
        }
      }
    }
    db_dispatch();
    dispatch();
    tick();
    time_t now = time(NULL);
    if (now != last_sweep) {                 /* idle connections: once a second is plenty */
      last_sweep = now;
      for (size_t i = 0; i < nconns; i++) {
        conn *c = conns[i];
        if ((c->st == READING || c->st == WRITING) && now - c->last > IDLE_SECS) { close_conn(c); i--; }
      }
    }
    empty_grave();
  }
  shutdown_all();
  return 0;
}
