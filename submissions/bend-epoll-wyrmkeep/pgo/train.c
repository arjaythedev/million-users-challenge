// The training run of this folder's build (build.sh, step 2). It runs the instrumented server
// for a few seconds under the challenge's request mix, so that clang can lay out the final
// build by what the server does per request. It is a build tool: nothing of it is in the server.
//
//   train --start start.sh --server BIN --log FILE [--seconds 8] [--conns 512] [--workers 2]
//         [--min-requests 50000] [--port P] [--canned DIR]
//
// One process, one thread, one epoll loop with three parts:
//
//   the child   start.sh (this folder's), so the workers get the flags they get on the box: one
//               block of start.sh sets them for both. Its environment is not this program's. It
//               holds PATH, HOME and LLVM_PROFILE_FILE as they came, and SERVER_BIN=BIN, WORKERS,
//               PORT, EXTRA_PORT=0, JWT_SECRET and DATABASE_URL, so no variable of the shell that
//               runs the build changes what is trained. PORT is a port the kernel just gave out
//               as free (--port names one instead). The workers listen on every address, as they
//               always do; this program talks to them on 127.0.0.1 only. Their output goes to --log.
//   the stub    a PostgreSQL for the server's own wire client (host/pgwire.c), on a 127.0.0.1
//               port the kernel picks: protocol 3.0, trust authentication, the five prepared
//               statements, Bind and Execute in pipelines, binary rows, one ReadyForQuery per
//               Sync. Trust, so the client's SCRAM code runs in no training and clang lays it out
//               as cold: it runs once per connection, at connect. The stub keeps the newest posts
//               and the next id: a create puts its text on top of the feed, as the real table
//               does, a like counts, and a like of a post that does not exist gets SQLSTATE 23503,
//               which rolls the batch back. An older post has a body built the way the
//               challenge's seed builds one (seed/generate.mjs: 1 to 3 sentences of 4 to 14 words).
//               Any local process can connect to the stub's port. A message of a type it does
//               not know, with a length out of range, or shorter than its own fields say ends
//               the training with exit 1: the stub reads nothing past a message's end.
//   the driver  --conns keep-alive connections, each a user of the challenge's load tool
//               (bench/mload/main.go): GET /feed, GET /posts/ID, a like in 15 turns of 100, a
//               new post in 2 of 100, every request with Accept-Encoding: gzip, writes with a
//               token this program signs with the JWT_SECRET it gave the server. No think time:
//               a connection sends its next request when the reply is in. Of 64 turns, 55 read a
//               post of the feed, 8 an older post and 1 a post that does not exist.
//
// The driver checks every reply's status against what the stub will answer, and its length
// against Content-Length. After --seconds of load it lets the replies in flight arrive, closes its
// connections and sends SIGTERM to start.sh, whose workers then write their counters
// (LLVM_PROFILE_FILE, set by build.sh) and exit.
//
// Exit 0 and one line on stdout ("train: N requests in S s ...") when every reply was the
// expected one, there were at least --min-requests of them and start.sh exited 0. Otherwise
// exit 1 and the reason on stderr, and build.sh builds without a profile.
//
// This program signals one process, start.sh, its own child, and only while it has not reaped
// it: until then the process id is start.sh's and no other process's (signal_child). Every way
// out of this program passes stop_child: SIGTERM to start.sh, which passes it to its workers
// and waits for them; five seconds; SIGKILL to start.sh; and the wait. A SIGTERM, SIGINT or
// SIGHUP to this program takes that way too. If this program is killed outright, the kernel
// sends start.sh a SIGTERM (PR_SET_PDEATHSIG). This program signals no process group, and it
// makes none: start.sh and its workers stay in the group this program is started in. That
// group is the one that build.sh's guard leads. The guard, a subshell of build.sh, kills the
// group, by its own process id, when the training is over. That is what ends a worker that
// has outlived start.sh. It writes --log and nothing else, and opens no socket but on
// 127.0.0.1.
//
// --canned DIR is not used by build.sh. It leaves the stub out and starts the server with
// --db canned --golden DIR (the challenge's test/golden), for the comparison of a training with
// the wire client against one without it. Replies are then checked for a 2xx status only.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SECRET "train-secret-of-this-build"
#define USERS 1000u        // tokens
#define SEED_POSTS 500000u  // the seed's posts: ids 1 to this
#define RING 256u           // created posts kept by id
#define FEED 20u

// ---- ways out ----

static pid_t                 child = -1;  // start.sh, from its fork until this program has reaped it
static volatile sig_atomic_t signalled;
static const char*           log_path;

static uint64_t now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

// A signal to start.sh, and to nothing else: child is the id fork returned, a number above 1,
// until the waitpid in child_ended has reaped start.sh. From then on child is -1 and no signal
// is sent. This is the only call of kill in this program.
static void signal_child(int sig) {
  if (child > 1) {
    kill(child, sig);
  }
}

// Whether start.sh has ended. If so, it is reaped and *st is its status.
static bool child_ended(int* st) {
  if (child <= 1 || waitpid(child, st, WNOHANG) != child) {
    return false;
  }
  child = -1;
  return true;
}

// Ends start.sh: SIGTERM, which it passes to its workers; SIGKILL after five seconds.
static void stop_child(void) {
  int      st    = 0;
  uint64_t until = now_ns() + 5000000000ull;
  signal_child(SIGTERM);
  while (child > 1 && !child_ended(&st)) {
    if (now_ns() > until) {
      signal_child(SIGKILL);
    }
    usleep(10000);
  }
}

static void die(const char* fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  fprintf(stderr, "train: ");
  vfprintf(stderr, fmt, a);
  fprintf(stderr, "\n");
  va_end(a);
  stop_child();
  exit(1);
}

static void on_signal(int sig) {
  signalled = sig;
}

static void* need(void* p, size_t n) {
  void* q = realloc(p, n);
  if (q == NULL) {
    die("out of memory");
  }
  return q;
}

// ---- bytes ----

typedef struct {
  char*  p;
  size_t n, off, cap;  // [off, n) not sent or not read yet
} Buf;

static char* buf_put(Buf* b, size_t n) {
  if (b->off == b->n) {
    b->off = b->n = 0;
  } else if (b->off > 0 && b->n + n > b->cap) {
    memmove(b->p, b->p + b->off, b->n - b->off);
    b->n -= b->off;
    b->off = 0;
  }
  if (b->n + n > b->cap) {
    b->cap = (b->n + n) * 2 + 4096;
    b->p   = need(b->p, b->cap);
  }
  b->n += n;
  return b->p + b->n - n;
}

static char* w16(char* p, uint32_t v) {
  p[0] = (char)(v >> 8);
  p[1] = (char)v;
  return p + 2;
}

static char* w32(char* p, uint32_t v) {
  p = w16(p, v >> 16);
  return w16(p, v & 0xFFFF);
}

static char* w64(char* p, uint64_t v) {
  p = w32(p, (uint32_t)(v >> 32));
  return w32(p, (uint32_t)v);
}

static uint32_t r16(const char* p) {
  const unsigned char* u = (const unsigned char*)p;
  return (uint32_t)u[0] << 8 | u[1];
}

static uint32_t r32(const char* p) {
  return r16(p) << 16 | r16(p + 2);
}

static uint64_t r64(const char* p) {
  return (uint64_t)r32(p) << 32 | r32(p + 4);
}

static uint64_t mix(uint64_t* s) {  // splitmix64
  uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
  z          = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z          = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

// ---- posts: the stub's table ----

static const char* const ADJ[20] = { "quiet", "brave", "clever", "sunny", "rusty", "swift", "lazy", "mellow", "sharp", "wild",
  "cosmic", "tiny", "golden", "silver", "happy", "grumpy", "curious", "lucky", "salty", "zesty" };
static const char* const NOUN[20] = { "otter", "falcon", "badger", "comet", "river", "maple", "pixel", "walrus", "ember",
  "glacier", "lantern", "meadow", "nebula", "orchid", "pebble", "quartz", "raven", "sparrow", "thistle", "willow" };
static const char* const OPENER[10] = { "Hot take:", "TIL", "Unpopular opinion:", "Note to self:", "PSA:", "Okay so", "Update:",
  "Honestly,", "Reminder:", "Question:" };
static const char* const END[6]     = { ".", ".", ".", "!", "?", "..." };
static const char* const WORD[]     = { "the", "a", "an", "and", "but", "or", "so", "because", "while", "after", "before", "just",
  "really", "very", "kind", "of", "sort", "of", "maybe", "probably", "definitely", "server", "database", "request", "latency",
  "cache", "index", "query", "benchmark", "droplet", "cpu", "memory", "disk", "network", "throughput", "users", "coffee",
  "morning", "weekend", "project", "deploy", "release", "bug", "feature", "test", "rewrite", "refactor", "commit", "branch",
  "merge", "review", "thinking", "building", "shipping", "learning", "reading", "writing", "debugging", "measuring", "watching",
  "waiting", "running", "walking", "fast", "slow", "small", "big", "cheap", "simple", "boring", "clever", "weird", "surprising",
  "honest", "careful", "lucky", "tired", "ready", "done", "broken", "today", "tonight", "tomorrow", "yesterday", "again",
  "finally", "never", "always", "sometimes", "usually", "rarely", "once", "twice", "this", "that", "these", "those", "here",
  "there", "everywhere", "nowhere", "something", "nothing", "everything", "anything", "someone", "nginx", "node", "postgres",
  "typescript", "express", "linux", "ubuntu", "systemd", "load", "steal", "swap", "kernel", "socket", "keepalive", "pool" };
#define WORDS (sizeof WORD / sizeof WORD[0])

typedef struct {
  uint32_t id, likes, body_n, author_n;
  int64_t  created_us;  // microseconds since 2000-01-01, as timestamptz travels
  char     body[512], author[48];
} Post;

static Post     ring[RING];                // created posts, by id % RING
static uint32_t next_id = SEED_POSTS + 1;  // the id the next create gets
static int64_t  epoch_us;                  // now, in microseconds since 2000-01-01

static uint32_t user_name(uint32_t uid, char* out, size_t cap) {
  return (uint32_t)snprintf(out, cap, "%s_%s_%u", ADJ[uid % 20], NOUN[uid / 20 % 20], uid);
}

// A post of the seed: its body as seed/generate.mjs writes one, from the id alone.
static void post_seed(uint32_t id, Post* p) {
  uint64_t s = id;
  size_t   n = 0;
  if (mix(&s) % 4 == 0) {
    n += (size_t)snprintf(p->body + n, sizeof p->body - n, "%s ", OPENER[mix(&s) % 10]);
  }
  uint32_t sentences = 1 + (uint32_t)(mix(&s) % 3);
  for (uint32_t i = 0; i < sentences; i += 1) {
    uint32_t words = 4 + (uint32_t)(mix(&s) % 11);
    size_t   at    = n;
    for (uint32_t k = 0; k < words && n + 24 < sizeof p->body; k += 1) {
      n += (size_t)snprintf(p->body + n, sizeof p->body - n, "%s%s", k > 0 ? " " : "", WORD[mix(&s) % WORDS]);
    }
    p->body[at] = (char)(p->body[at] - 'a' + 'A');
    n += (size_t)snprintf(p->body + n, sizeof p->body - n, "%s%s", END[mix(&s) % 6], i + 1 < sentences ? " " : "");
  }
  p->id         = id;
  p->body_n     = (uint32_t)n;
  p->likes      = (uint32_t)(mix(&s) % 20);
  p->created_us = epoch_us - (int64_t)(SEED_POSTS - id) * 63000000ll - 1000000;
  p->author_n   = user_name(1 + (uint32_t)(mix(&s) % 50000), p->author, sizeof p->author);
}

// The post with this id, or false: ids 1 to next_id - 1 exist.
static bool post_get(uint32_t id, Post* p) {
  if (id < 1 || id >= next_id) {
    return false;
  }
  if (id > SEED_POSTS && next_id - id <= RING) {
    *p = ring[id % RING];
  } else {
    post_seed(id, p);
  }
  return true;
}

static uint32_t feed_id(uint32_t k) {  // the k-th newest post's id, k under FEED
  return next_id - 1 - k;
}

// ---- the stub ----

typedef struct {
  int      fd;
  bool     started, aborted, bound;
  int      kind;    // of the statement the last Bind named
  uint64_t a, b;    // its int8 parameters
  char     text[512];
  uint32_t text_n;
  uint32_t binds;   // since the last Sync
  Buf      in, out;
} Pc;

static const char* const STMT[5] = { "s_one", "s_feed", "s_post", "s_ins", "s_like" };
static int               ep, stub_fd = -1, stub_port;
static Pc**              pcs;
static uint32_t          npc;
static uint64_t          stub_batches, stub_statements, stub_refused;
static Buf               feed_wire;  // the feed's DataRows and CommandComplete
static bool              feed_stale = true;

static void watch(int fd, uint32_t events, uint64_t data, int op) {
  struct epoll_event ev = { .events = events, .data.u64 = data };
  if (epoll_ctl(ep, op, fd, &ev) != 0) {
    die("epoll_ctl: %s", strerror(errno));
  }
}

static void nonblock(int fd) {
  int one = 1;
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

// A message's type byte and length word; the caller fills body bytes after.
static char* msg(Buf* b, char type, size_t body) {
  char* p = buf_put(b, 5 + body);
  p[0]    = type;
  return w32(p + 1, (uint32_t)(4 + body));
}

static void msg_str(Buf* b, char type, const char* s) {
  size_t n = strlen(s) + 1;
  memcpy(msg(b, type, n), s, n);
}

static void row_post(Buf* b, const Post* p) {
  char* q = msg(b, 'D', 2 + 4 + 8 + 4 + p->body_n + 4 + 8 + 4 + p->author_n + 4 + 4);
  q       = w16(q, 5);
  q       = w64(w32(q, 8), p->id);
  q       = w32(q, p->body_n);
  memcpy(q, p->body, p->body_n);
  q = w64(w32(q + p->body_n, 8), (uint64_t)p->created_us);
  q = w32(q, p->author_n);
  memcpy(q, p->author, p->author_n);
  w32(w32(q + p->author_n, 4), p->likes);
}

static void feed_build(void) {
  Post p;
  feed_wire.n = feed_wire.off = 0;
  for (uint32_t k = 0; k < FEED; k += 1) {
    post_get(feed_id(k), &p);
    row_post(&feed_wire, &p);
  }
  msg_str(&feed_wire, 'C', "SELECT 20");
  feed_stale = false;
}

static bool like_repeat(uint64_t uid, uint64_t pid) {  // the driver expects 200 for these, 201 for the rest
  return ((uid ^ pid) & 7) == 0;
}

// The answer to the Execute of the statement the last Bind named.
static void stub_execute(Pc* c) {
  Buf* o = &c->out;
  Post p;
  stub_statements += 1;
  switch (c->kind) {
  case 0:
    w32(w32(w16(msg(o, 'D', 2 + 4 + 4), 1), 4), 1);
    msg_str(o, 'C', "SELECT 1");
    break;
  case 1:
    if (feed_stale) {
      feed_build();
    }
    memcpy(buf_put(o, feed_wire.n), feed_wire.p, feed_wire.n);
    break;
  case 2:
    if (c->a <= 0xFFFFFFFFull && post_get((uint32_t)c->a, &p)) {
      row_post(o, &p);
      msg_str(o, 'C', "SELECT 1");
    } else {
      msg_str(o, 'C', "SELECT 0");
    }
    break;
  case 3: {
    Post* n       = &ring[next_id % RING];
    n->id         = next_id;
    n->likes      = 0;
    n->created_us = epoch_us + (int64_t)(next_id - SEED_POSTS) * 1000;
    n->body_n     = c->text_n;
    memcpy(n->body, c->text, c->text_n);
    n->author_n = user_name((uint32_t)c->a, n->author, sizeof n->author);
    next_id += 1;
    feed_stale = true;
    w64(w32(w64(w32(w16(msg(o, 'D', 2 + 4 + 8 + 4 + 8), 2), 8), n->id), 8), (uint64_t)n->created_us);
    msg_str(o, 'C', "INSERT 0 1");
    break;
  }
  default:
    if (c->b < 1 || c->b >= next_id) {  // no such post: the foreign key fails, and the rest of the pipeline is skipped
      static const char err[] = "SERROR\0VERROR\0C23503\0Minsert or update on table \"likes\" violates foreign key constraint\0";
      memcpy(msg(o, 'E', sizeof err), err, sizeof err);
      c->aborted = true;
      stub_refused += 1;
    } else if (like_repeat(c->a, c->b)) {
      msg_str(o, 'C', "INSERT 0 0");
    } else {
      if (c->b > SEED_POSTS && next_id - c->b <= RING) {
        ring[c->b % RING].likes += 1;
        feed_stale = true;
      }
      msg_str(o, 'C', "INSERT 0 1");
    }
  }
}

// The next n bytes of a message's body. The stub reads nothing past a message's end: a
// message that is shorter than its own fields say ends the training (exit 1), and build.sh
// builds without a profile.
static const char* take(const char** p, const char* end, size_t n) {
  const char* at = *p;
  if ((size_t)(end - at) < n) {
    die("the stub got a Bind that ends before its fields do");
  }
  *p = at + n;
  return at;
}

// Bind: the portal, the statement, the parameter formats, the parameters. The result formats
// after them are not read.
static void stub_bind(Pc* c, const char* body, uint32_t len) {
  const char* end    = body + len;
  const char* portal = memchr(body, 0, len);
  const char* name   = portal != NULL ? portal + 1 : end;
  const char* z      = memchr(name, 0, (size_t)(end - name));
  if (z == NULL) {
    die("the stub got a Bind of %u bytes without a statement's name", len);
  }
  c->kind = -1;
  for (int k = 0; k < 5; k += 1) {
    if (!strcmp(name, STMT[k])) {
      c->kind = k;
    }
  }
  if (c->kind < 0) {
    die("the stub has no statement %.40s", name);
  }
  const char* p  = z + 1;
  uint32_t    nf = r16(take(&p, end, 2));
  take(&p, end, 2 * (size_t)nf);
  uint32_t np   = r16(take(&p, end, 2));
  uint64_t v[2] = { 0, 0 };
  c->text_n     = 0;
  if (np > 2) {
    die("the stub got %u parameters for %s", np, name);
  }
  for (uint32_t i = 0; i < np; i += 1) {
    uint32_t    n   = r32(take(&p, end, 4));
    const char* val = take(&p, end, n);  // a NULL parameter (length -1) ends here too
    if (c->kind == 3 && i == 1) {
      c->text_n = n < sizeof c->text ? n : (uint32_t)sizeof c->text;
      memcpy(c->text, val, c->text_n);
    } else if (n == 8) {
      v[i] = r64(val);
    } else {
      die("the stub wants an int8 for parameter %u of %s, and got %u bytes", i + 1, name, n);
    }
  }
  c->a     = v[0];
  c->b     = v[1];
  c->bound = true;
}

static void stub_flush(Pc* c, uint32_t i) {
  Buf* o = &c->out;
  while (o->off < o->n) {
    ssize_t k = send(c->fd, o->p + o->off, o->n - o->off, MSG_NOSIGNAL);
    if (k > 0) {
      o->off += (size_t)k;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      watch(c->fd, EPOLLIN | EPOLLOUT, 2ull << 32 | i, EPOLL_CTL_MOD);
      return;
    } else if (errno != EINTR) {
      o->off = o->n;  // the client has gone: its end of file closes the connection
    }
  }
  watch(c->fd, EPOLLIN, 2ull << 32 | i, EPOLL_CTL_MOD);
}

static void stub_close(Pc* c) {
  close(c->fd);
  free(c->in.p);
  free(c->out.p);
  c->fd = -1;
}

// Every whole message in c->in.
static void stub_messages(Pc* c, uint32_t i) {
  Buf* in = &c->in;
  for (;;) {
    size_t      have = in->n - in->off;
    const char* p    = in->p + in->off;
    if (!c->started) {  // no type byte: the length, then the protocol or a request code
      if (have < 8) {
        break;
      }
      uint32_t len = r32(p), code = r32(p + 4);
      if (len < 8 || len > 65536) {  // under its own two words, or no startup message
        die("the stub got a startup message with a length of %u", len);
      }
      if (have < len) {
        break;
      }
      in->off += len;
      if (code == 80877103 || code == 80877104) {  // SSLRequest, GSSENCRequest: no
        *buf_put(&c->out, 1) = 'N';
      } else if (code == 196608) {
        static const char par[] = "integer_datetimes\0on";
        c->started              = true;
        w32(msg(&c->out, 'R', 4), 0);  // AuthenticationOk
        memcpy(msg(&c->out, 'S', sizeof par), par, sizeof par);
        w32(w32(msg(&c->out, 'K', 8), 7000 + i), 0x5eed + i);
        *msg(&c->out, 'Z', 1) = 'I';
      } else {  // a CancelRequest, or a protocol the stub does not speak
        stub_close(c);
        return;
      }
      continue;
    }
    if (have < 5) {
      break;
    }
    uint32_t total = r32(p + 1);  // the length word counts itself
    if (total < 4 || total > 1u << 24) {
      die("the stub got a message %c with a length of %u", p[0] >= ' ' && p[0] <= '~' ? p[0] : '?', total);
    }
    if (have < 1 + (size_t)total) {
      break;
    }
    uint32_t len = total - 4;
    in->off += 5 + len;
    switch (p[0]) {
    case 'P':
      msg(&c->out, '1', 0);
      break;
    case 'B':
      if (!c->aborted) {
        stub_bind(c, p + 5, len);
        msg(&c->out, '2', 0);
        c->binds += 1;
      }
      break;
    case 'E':
      if (!c->aborted) {
        if (!c->bound) {
          die("the stub got an Execute without a Bind");
        }
        c->bound = false;
        stub_execute(c);
      }
      break;
    case 'S':
      stub_batches += c->binds > 0;
      c->binds      = 0;
      c->aborted    = false;
      *msg(&c->out, 'Z', 1) = 'I';
      break;
    case 'X':
      stub_close(c);
      return;
    default:
      die("the stub does not know the message type %c the server sent", p[0]);
    }
  }
  stub_flush(c, i);
}

static void stub_event(uint32_t i, uint32_t events) {
  Pc* c = pcs[i];
  if (c->fd < 0) {
    return;
  }
  if (events & EPOLLIN) {
    for (;;) {
      char*   p = buf_put(&c->in, 65536);
      ssize_t k = recv(c->fd, p, 65536, 0);
      c->in.n -= 65536 - (k > 0 ? (size_t)k : 0);
      if (k > 0) {
        continue;
      }
      if (k == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        stub_close(c);
        return;
      }
      break;
    }
    stub_messages(c, i);
  } else if (events & EPOLLOUT) {
    stub_flush(c, i);
  } else {
    stub_close(c);
  }
}

static void stub_accept(void) {
  for (;;) {
    int fd = accept(stub_fd, NULL, NULL);
    if (fd < 0) {
      return;
    }
    nonblock(fd);
    pcs       = need(pcs, (npc + 1) * sizeof *pcs);
    Pc* c     = need(NULL, sizeof *c);
    *c        = (Pc){ .fd = fd };
    pcs[npc]  = c;
    watch(fd, EPOLLIN, 2ull << 32 | npc, EPOLL_CTL_ADD);
    npc += 1;
  }
}

// A listener on 127.0.0.1 and a port the kernel picks; the port.
static int listen_local(int* fd_out) {
  struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
  socklen_t          n = sizeof a;
  int                fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0 || bind(fd, (struct sockaddr*)&a, sizeof a) != 0 || listen(fd, 1024) != 0
    || getsockname(fd, (struct sockaddr*)&a, &n) != 0) {
    die("no listener on 127.0.0.1: %s", strerror(errno));
  }
  *fd_out = fd;
  return ntohs(a.sin_port);
}

// ---- the driver ----

enum { OP_HEALTH, OP_FEED, OP_POST, OP_LIKE, OP_CREATE, OPS };
enum { H_CONNECTING, H_IDLE, H_WAIT };

typedef struct {
  int      fd;
  uint8_t  state, op, step;
  int      expect;  // the status the reply must have
  uint32_t user, uid, pid, turns;  // user: which of the USERS; uid: its id
  uint64_t rng;
  char     req[1024];
  uint32_t req_n, req_off;
  Buf      in;
} Hc;

static Hc*         hcs;
static uint32_t    conns = 512, waiting, healthy;
static uint64_t    done[OPS], not_found[OPS], repeats;
static bool        loading, canned;
static char        tokens[USERS][256];
static int         http_port;

static void b64url(const unsigned char* in, size_t n, char* out) {
  static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  size_t            o     = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
    out[o++]   = abc[v >> 18 & 63];
    out[o++]   = abc[v >> 12 & 63];
    if (i + 1 < n) out[o++] = abc[v >> 6 & 63];
    if (i + 2 < n) out[o++] = abc[v & 63];
  }
  out[o] = 0;
}

static uint32_t user_id(uint32_t k) {  // the k-th of the USERS users
  return 1 + k * 7919u % 50000u;
}

// An HS256 token as the challenge's seed signs them: sub, username, iat, exp.
static void token(uint32_t uid, char* out) {
  char          name[48], pay[160], sig[64];
  unsigned char mac[32];
  unsigned int  mn = sizeof mac;
  time_t        t  = time(NULL);
  user_name(uid, name, sizeof name);
  int    pn = snprintf(pay, sizeof pay, "{\"sub\":\"%u\",\"username\":\"%s\",\"iat\":%lld,\"exp\":%lld}", uid, name, (long long)t,
    (long long)t + 3600);
  size_t n  = (size_t)sprintf(out, "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.");  // {"alg":"HS256","typ":"JWT"}
  b64url((const unsigned char*)pay, (size_t)pn, out + n);
  n = strlen(out);
  if (HMAC(EVP_sha256(), SECRET, (int)strlen(SECRET), (const unsigned char*)out, n, mac, &mn) == NULL) {
    die("HMAC failed");
  }
  b64url(mac, sizeof mac, sig);
  sprintf(out + n, ".%s", sig);
}

static void http_send(Hc* h, uint32_t i) {
  while (h->req_off < h->req_n) {
    ssize_t k = send(h->fd, h->req + h->req_off, h->req_n - h->req_off, MSG_NOSIGNAL);
    if (k > 0) {
      h->req_off += (uint32_t)k;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      watch(h->fd, EPOLLIN | EPOLLOUT, 3ull << 32 | i, EPOLL_CTL_MOD);
      return;
    } else if (errno != EINTR) {
      die("the server closed connection %u: %s", i, strerror(errno));
    }
  }
}

// The request of h->op, as mload writes one, and the status its reply must have.
static void http_request(Hc* h, uint32_t i) {
  static const char* const HEAD = " HTTP/1.1\r\nHost: 127.0.0.1\r\nUser-Agent: mload\r\nAccept-Encoding: gzip\r\n";
  const char*              tok  = tokens[h->user];
  char                     body[160], name[48];
  int                      n = 0, bn;
  bool                     gone = !canned && h->pid >= next_id;
  switch (h->op) {
  case OP_HEALTH:
    n         = snprintf(h->req, sizeof h->req, "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nUser-Agent: mload\r\n\r\n");
    h->expect = 200;
    break;
  case OP_FEED:
    n         = snprintf(h->req, sizeof h->req, "GET /feed%s\r\n", HEAD);
    h->expect = 200;
    break;
  case OP_POST:
    n         = snprintf(h->req, sizeof h->req, "GET /posts/%u%s\r\n", h->pid, HEAD);
    h->expect = gone ? 404 : 200;
    break;
  case OP_LIKE:
    n = snprintf(h->req, sizeof h->req, "POST /posts/%u/like%sAuthorization: Bearer %s\r\nContent-Length: 0\r\n\r\n", h->pid, HEAD, tok);
    h->expect = gone ? 404 : like_repeat(h->uid, h->pid) ? 200 : 201;
    break;
  default:
    user_name(h->uid, name, sizeof name);
    bn = snprintf(body, sizeof body, "{\"body\":\"%s says hi at 2026-11-10T00:00:%02u.%03uZ (VU %u, iter %u)\"}", name, h->turns % 60,
      i % 1000, i + 1, h->turns);
    n  = snprintf(h->req, sizeof h->req,
       "POST /posts%sAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s", HEAD, tok, bn, body);
    h->expect = 201;
  }
  if (n <= 0 || (size_t)n >= sizeof h->req) {
    die("a request of %d bytes does not fit", n);
  }
  h->req_n   = (uint32_t)n;
  h->req_off = 0;
  h->state   = H_WAIT;
  waiting += 1;
  http_send(h, i);
}

// The next request of the user's turn, or none once the load is over.
static void http_next(Hc* h, uint32_t i) {
  h->state = H_IDLE;
  while (loading) {
    uint32_t r = (uint32_t)(mix(&h->rng) >> 33);
    switch (h->step) {
    case 0:
      h->turns += 1;
      h->user = r % USERS;
      h->uid  = user_id(h->user);
      r      = (uint32_t)(mix(&h->rng) >> 33);
      if (canned) {
        h->pid = r % 3 == 0 ? 1 : r % 3 == 1 ? 250000 : 500000;
      } else if (r % 64 == 0) {
        h->pid = 600000000 + r % 1000;  // no such post
      } else if (r % 64 <= 8) {
        h->pid = 1 + (r >> 6) % SEED_POSTS;
      } else {
        h->pid = feed_id((r >> 6) % FEED);
      }
      h->step = 1;
      h->op   = OP_FEED;
      break;
    case 1:
      h->step = 2;
      h->op   = OP_POST;
      break;
    case 2:
      h->step = 3;
      if (r % 100 >= 15) {
        continue;
      }
      h->op = OP_LIKE;
      break;
    default:
      h->step = 0;
      if (r % 100 >= 2) {
        continue;
      }
      h->op = OP_CREATE;
    }
    http_request(h, i);
    return;
  }
}

// A whole reply, or not yet: its status line, its Content-Length and that many bytes after the head.
static void http_reply(Hc* h, uint32_t i) {
  Buf* b = &h->in;
  for (;;) {
    char*   p = buf_put(b, 16384);
    ssize_t k = recv(h->fd, p, 16384, 0);
    b->n -= 16384 - (k > 0 ? (size_t)k : 0);
    if (k > 0) {
      continue;
    }
    if (k == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
      die("the server closed connection %u in a %s", i, h->state == H_WAIT ? "request" : "pause");
    }
    break;
  }
  if (h->state != H_WAIT) {
    if (b->n > 0) {
      die("connection %u got bytes it did not ask for", i);
    }
    return;
  }
  const char* head = memmem(b->p, b->n, "\r\n\r\n", 4);
  if (head == NULL) {
    return;
  }
  size_t      hn = (size_t)(head - b->p) + 4;
  const char* cl = memmem(b->p, hn, "\r\nContent-Length: ", 18);
  if (b->n < 12 || memcmp(b->p, "HTTP/1.1 ", 9) != 0 || cl == NULL) {
    die("connection %u got a reply the driver cannot read: %.40s", i, b->p);
  }
  size_t body = (size_t)strtoul(cl + 18, NULL, 10);
  if (b->n < hn + body) {
    return;
  }
  int status = atoi(b->p + 9);
  if (b->n > hn + body) {
    die("connection %u got %zu bytes after its reply", i, b->n - hn - body);
  }
  if (canned ? status / 100 != 2 : status != h->expect) {
    static const char* const NAME[OPS] = { "GET /health", "GET /feed", "GET /posts/ID", "POST /posts/ID/like", "POST /posts" };
    die("%s (post %u, user %u) got status %d, not %d", NAME[h->op], h->pid, h->uid, status, h->expect);
  }
  b->n = b->off = 0;
  waiting -= 1;
  done[h->op] += 1;
  not_found[h->op] += status == 404;
  repeats += h->op == OP_LIKE && status == 200;
  if (h->op == OP_HEALTH) {
    healthy += 1;
    h->state = H_IDLE;
  } else {
    http_next(h, i);
  }
}

static void http_event(uint32_t i, uint32_t events) {
  Hc* h = &hcs[i];
  if (h->state == H_CONNECTING) {
    int       err = 0;
    socklen_t n   = sizeof err;
    getsockopt(h->fd, SOL_SOCKET, SO_ERROR, &err, &n);
    if (err != 0 || (events & (EPOLLERR | EPOLLHUP))) {
      die("connection %u to 127.0.0.1:%d failed: %s", i, http_port, strerror(err));
    }
    watch(h->fd, EPOLLIN, 3ull << 32 | i, EPOLL_CTL_MOD);
    h->op = OP_HEALTH;
    http_request(h, i);
    return;
  }
  if (events & EPOLLOUT) {
    watch(h->fd, EPOLLIN, 3ull << 32 | i, EPOLL_CTL_MOD);
    http_send(h, i);
  }
  if (events & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
    http_reply(h, i);
  }
}

static void http_open(uint32_t i) {
  struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)http_port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
  Hc*                h = &hcs[i];
  h->fd                = socket(AF_INET, SOCK_STREAM, 0);
  if (h->fd < 0) {
    die("socket: %s", strerror(errno));
  }
  nonblock(h->fd);
  h->rng   = 0x5eed0000ull + i;
  h->state = H_CONNECTING;
  if (connect(h->fd, (struct sockaddr*)&a, sizeof a) != 0 && errno != EINPROGRESS) {
    die("connect to 127.0.0.1:%d: %s", http_port, strerror(errno));
  }
  watch(h->fd, EPOLLOUT, 3ull << 32 | i, EPOLL_CTL_ADD);
}

// ---- the child ----

// The log's last line, for a reason; and how many of its lines start with want.
static uint32_t log_lines(const char* want, char* last, size_t cap) {
  FILE*    f = fopen(log_path, "r");
  char     line[512];
  uint32_t n = 0;
  last[0]    = 0;
  if (f == NULL) {
    return 0;
  }
  while (fgets(line, sizeof line, f) != NULL) {
    n += !strncmp(line, want, strlen(want));
    line[strcspn(line, "\n")] = 0;
    if (line[0] != 0) {
      snprintf(last, cap, "%s", line);
    }
  }
  fclose(f);
  return n;
}

static int free_port(void) {
  int fd, port = listen_local(&fd);
  close(fd);
  return port;
}

static void spawn(const char* start, const char* server, uint32_t workers, const char* golden) {
  char port[16], count[16], url[96];
  snprintf(port, sizeof port, "%d", http_port);
  snprintf(count, sizeof count, "%u", workers);
  snprintf(url, sizeof url, "postgres://train:train@127.0.0.1:%d/train", stub_port);
  pid_t parent = getpid();
  child        = fork();
  if (child < 0) {
    die("fork: %s", strerror(errno));
  }
  if (child == 0) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    int fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (getppid() != parent || fd < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0) {
      _exit(127);
    }
    signal(SIGPIPE, SIG_DFL);
    static const char* const KEEP[3] = { "PATH", "HOME", "LLVM_PROFILE_FILE" };
    char*                    kept[3];
    for (int k = 0; k < 3; k += 1) {
      kept[k] = getenv(KEEP[k]) != NULL ? strdup(getenv(KEEP[k])) : NULL;
    }
    clearenv();
    for (int k = 0; k < 3; k += 1) {
      if (kept[k] != NULL) {
        setenv(KEEP[k], kept[k], 1);
      }
    }
    setenv("PORT", port, 1);
    setenv("EXTRA_PORT", "0", 1);
    setenv("WORKERS", count, 1);
    setenv("SERVER_BIN", server, 1);
    setenv("JWT_SECRET", SECRET, 1);
    setenv("DATABASE_URL", url, 1);
    if (golden != NULL) {
      setenv("DB", "canned", 1);
      execlp("bash", "bash", start, "--golden", golden, (char*)NULL);
    } else {
      execlp("bash", "bash", start, (char*)NULL);
    }
    _exit(127);
  }
}

// The sockets' events, for at most ms; ends the run on a signal.
static void events(int ms) {
  struct epoll_event evs[256];
  int                n = epoll_wait(ep, evs, 256, ms);
  if (signalled) {
    die("stopped by signal %d", (int)signalled);
  }
  for (int k = 0; k < n; k += 1) {
    uint32_t kind = (uint32_t)(evs[k].data.u64 >> 32), i = (uint32_t)evs[k].data.u64;
    if (kind == 1) {
      stub_accept();
    } else if (kind == 2) {
      stub_event(i, evs[k].events);
    } else {
      http_event(i, evs[k].events);
    }
  }
}

static void stopped_by_itself(int st) __attribute__((noreturn));
static void stopped_by_itself(int st) {
  char last[512];
  log_lines("", last, sizeof last);
  die("the server stopped by itself (start.sh: %s %d): %s", WIFEXITED(st) ? "exit" : "signal",
    WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st), last);
}

// One pass of the loop: the events, and the end of the run when start.sh has exited.
static void turn(int ms) {
  int st;
  events(ms);
  if (child_ended(&st)) {
    stopped_by_itself(st);
  }
}

// Whether start.sh ended because the port was taken: a worker's bind failed (exit 98,
// "listen on --port"), or start.sh found a listener there (exit 2).
static bool port_taken(int st) {
  char last[512];
  log_lines("", last, sizeof last);
  return WIFEXITED(st) && (WEXITSTATUS(st) == 98 || (WEXITSTATUS(st) == 2 && strstr(last, "already has a listener") != NULL));
}

int main(int argc, char** argv) {
  const char *start = NULL, *server = NULL, *golden = NULL;
  double      seconds = 8;
  uint32_t    workers = 2, forced = 0;
  uint64_t    least = 50000;
  for (int i = 1; i + 1 < argc; i += 2) {
    const char *k = argv[i], *v = argv[i + 1];
    if (!strcmp(k, "--start")) start = v;
    else if (!strcmp(k, "--server")) server = v;
    else if (!strcmp(k, "--log")) log_path = v;
    else if (!strcmp(k, "--seconds")) seconds = atof(v);
    else if (!strcmp(k, "--conns")) conns = (uint32_t)atoi(v);
    else if (!strcmp(k, "--workers")) workers = (uint32_t)atoi(v);
    else if (!strcmp(k, "--min-requests")) least = strtoull(v, NULL, 10);
    else if (!strcmp(k, "--port")) forced = (uint32_t)atoi(v);
    else if (!strcmp(k, "--canned")) golden = v;
    else start = NULL, i = argc;
  }
  if (argc % 2 == 0 || start == NULL || server == NULL || log_path == NULL || seconds <= 0 || conns < 1 || conns > 20000 || workers < 1
    || workers > 64 || forced > 65535) {
    fprintf(stderr, "usage: train --start start.sh --server BIN --log FILE [--seconds 8] [--conns 512] [--workers 2]"
                    " [--min-requests 50000] [--port P] [--canned DIR]\n");
    return 2;
  }
  canned   = golden != NULL;
  epoch_us = ((int64_t)time(NULL) - 946684800) * 1000000;
  struct sigaction sa = { .sa_handler = on_signal };
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);
  ep = epoll_create1(EPOLL_CLOEXEC);
  if (ep < 0) {
    die("epoll_create1: %s", strerror(errno));
  }
  if (!canned) {
    stub_port = listen_local(&stub_fd);
    fcntl(stub_fd, F_SETFL, O_NONBLOCK);
    fcntl(stub_fd, F_SETFD, FD_CLOEXEC);
    watch(stub_fd, EPOLLIN, 1ull << 32, EPOLL_CTL_ADD);
  }
  for (uint32_t k = 0; k < USERS; k += 1) {
    token(user_id(k), tokens[k]);
  }

  // the server, up: every worker has written its start line. The port the kernel gave out as
  // free can be taken before the workers bind it, as the source port of somebody's outgoing
  // connection: then the next port, five times at most. A port named with --port is not replaced.
  char     last[512];
  uint64_t t0 = now_ns();
  for (int tries = 1;; tries += 1) {
    int st;
    http_port = forced != 0 ? (int)forced : free_port();
    spawn(start, server, workers, golden);
    while (child > 0 && log_lines("bend-epoll: worker ", last, sizeof last) < workers) {
      if (now_ns() - t0 > 20000000000ull) {
        die("the server was not up after 20 s: %s", last);
      }
      events(20);
      if (child_ended(&st) && (forced != 0 || tries == 5 || !port_taken(st))) {
        stopped_by_itself(st);
      }
    }
    if (child > 0) {
      break;
    }
  }

  // the connections, each through one GET /health
  hcs = need(NULL, conns * sizeof *hcs);
  memset(hcs, 0, conns * sizeof *hcs);
  for (uint32_t i = 0; i < conns; i += 1) {
    http_open(i);
  }
  while (healthy < conns) {
    if (now_ns() - t0 > 40000000000ull) {
      log_lines("", last, sizeof last);
      die("%u of %u connections were answered after 40 s: %s", healthy, conns, last);
    }
    turn(20);
  }

  // the load, then the replies in flight
  uint64_t l0 = now_ns(), span = (uint64_t)(seconds * 1e9), l1;
  loading     = true;
  for (uint32_t i = 0; i < conns; i += 1) {
    http_next(&hcs[i], i);
  }
  while (now_ns() - l0 < span) {
    turn(20);
  }
  loading = false;
  while (waiting > 0) {
    if (now_ns() - l0 > span + 10000000000ull) {
      die("%u requests had no reply 10 s after the load ended", waiting);
    }
    turn(20);
  }
  l1 = now_ns();
  for (uint32_t i = 0; i < conns; i += 1) {
    close(hcs[i].fd);
  }

  // the workers write their counters as they exit
  int      st = 0;
  uint64_t s0 = now_ns();
  signal_child(SIGTERM);
  while (!child_ended(&st)) {
    if (signalled || now_ns() - s0 > 15000000000ull) {
      die("start.sh did not exit within 15 s of its SIGTERM");
    }
    struct epoll_event evs[64];
    int                n = epoll_wait(ep, evs, 64, 20);
    for (int k = 0; k < n; k += 1) {
      if (evs[k].data.u64 >> 32 == 2) {
        stub_event((uint32_t)evs[k].data.u64, evs[k].events);
      }
    }
  }
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    log_lines("", last, sizeof last);
    die("start.sh ended with %s %d after its SIGTERM: %s", WIFEXITED(st) ? "exit" : "signal",
      WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st), last);
  }

  uint64_t all = done[OP_FEED] + done[OP_POST] + done[OP_LIKE] + done[OP_CREATE];
  if (all < least) {
    die("%llu requests in %.1f s, under the %llu a profile is taken from", (unsigned long long)all, (double)(l1 - l0) / 1e9,
      (unsigned long long)least);
  }
  printf("train: %llu requests in %.2f s (feed %llu, post %llu with %llu not found, like %llu with %llu repeats and %llu not found,"
         " create %llu), %u connections, %u workers",
    (unsigned long long)all, (double)(l1 - l0) / 1e9, (unsigned long long)done[OP_FEED], (unsigned long long)done[OP_POST],
    (unsigned long long)not_found[OP_POST], (unsigned long long)done[OP_LIKE], (unsigned long long)repeats,
    (unsigned long long)not_found[OP_LIKE], (unsigned long long)done[OP_CREATE], conns, workers);
  if (canned) {
    printf("; --db canned, no stub\n");
  } else {
    printf("; stub: %u connections, %llu batches of %.1f statements, %llu refused\n", npc, (unsigned long long)stub_batches,
      stub_batches > 0 ? (double)stub_statements / (double)stub_batches : 0.0, (unsigned long long)stub_refused);
  }
  return 0;
}
