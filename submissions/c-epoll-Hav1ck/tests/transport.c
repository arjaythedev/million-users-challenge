// Deterministic transport/lifecycle tests; no database, listeners or load generator.
#define _GNU_SOURCE
#include <sys/epoll.h>
#include <unistd.h>
#include <sys/uio.h>
#include <errno.h>
// Count real control calls, not time. Faults and short writes apply only to the
// selected descriptor; all successful operations still use kernel sockets.
static unsigned ctl_mods;
static int fail_mod_fd = -1, short_write_fd = -1, interrupt_write, interrupt_writev_fd = -1;
static int checked_epoll_ctl(int ep, int op, int fd, struct epoll_event *ev) {
  if (op == EPOLL_CTL_MOD) {
    ctl_mods++;
    if (fd == fail_mod_fd) { errno = ENOENT; return -1; }
  }
  return epoll_ctl(ep, op, fd, ev);
}
static ssize_t checked_write(int fd, const void *buf, size_t len) {
  if (fd == short_write_fd) {
    if (interrupt_write) { interrupt_write = 0; errno = EINTR; return -1; }
    if (len > 31) len = 31;
  }
  return write(fd, buf, len);
}
static ssize_t checked_writev(int fd, const struct iovec *iov, int n) {
  if (fd == interrupt_writev_fd) { interrupt_writev_fd = -1; errno = EINTR; return -1; }
  return writev(fd, iov, n);
}
#define writev checked_writev
#define epoll_ctl checked_epoll_ctl
#define write checked_write
#define main feed_server_main
#include "../server.c"
#undef main

static th_t t;
static int client(int *peer) {
  int s[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, s));
  assert(s[0] < maxfd); assert(!conns[s[0]].open);
  conns[s[0]].open = 1; *peer = s[1];
  ep_set(t.ep, EPOLL_CTL_ADD, s[0], s[0], EPOLLIN);
  return s[0];
}
static int step(void) {
  struct epoll_event ev[16]; int n = epoll_wait(t.ep, ev, 16, 0); assert(n >= 0);
  for (int i = 0; i < n; i++) client_event(&t, ev[i].data.u64, ev[i].events);
  close_pending(&t); return n;
}
static size_t drain(int fd, char *b, size_t cap) {
  size_t n = 0;
  while (n < cap) {
    ssize_t r = read(fd, b + n, cap - n);
    if (!r) break;
    if (r < 0) { assert(errno == EAGAIN); break; }
    n += r;
  }
  return n;
}
static void done(int fd, int peer) { conn_close(&t, fd); close_pending(&t); close(peer); }
static void msg(int fd, char ty, const char *pl, size_t n) {
  char b[64]; assert(n + 5 <= sizeof b); b[0] = ty; w32(b + 1, n + 4);
  memcpy(b + 5, pl, n); assert(write(fd, b, n + 5) == (ssize_t)n + 5);
}
static int wire(pg_t *p) {
  int s[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, s));
  p->sock = s[0]; p->icap = 65536; p->ib = malloc(p->icap); return s[1];
}
static void wire_done(pg_t *p, int peer) {
  assert(!p->n && !p->nheld); close(p->sock); close(peer);
  free(p->ib); free(p->ob); free(p->q); memset(p, 0, sizeof *p); p->sock = -1;
}
static void health_reply(pg_t *p, int peer) {
  assert(p->unsynced); pg_sync(p);
  msg(peer, 'C', "SELECT 1", 9); msg(peer, 'Z', "I", 1); pg_read(&t, p);
}

static void deferred_batch(void) {
  int peer, fd = client(&peer); uint32_t generation = conns[fd].gen;
  assert(write(peer, "GET /unknown HTTP/1.1\r\n\r\n", 25) == 25);
  struct epoll_event ev; assert(epoll_wait(t.ep, &ev, 1, 0) == 1);
  assert(ev.data.u64 == (uint64_t)fd);
  q_t stale = {.fd = fd, .gen = generation, .kind = Q_LIKE};
  conn_close(&t, fd); conn_close(&t, fd); // idempotent, no duplicate list entry
  assert(!conns[fd].open && conns[fd].gen != generation);
  assert(fcntl(fd, F_GETFD) >= 0); // still allocated while a stale batch entry exists
  int fresh[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fresh));
  assert(fresh[0] != fd && fresh[1] != fd);
  client_event(&t, ev.data.u64, ev.events); assert(!conns[fd].open);
  close(fresh[0]); close(fresh[1]); close_pending(&t);
  assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);

  // Simulate another worker accepting exactly this fd AFTER the previous batch.
  th_t other = {.ep = epoll_create1(EPOLL_CLOEXEC)};
  assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fresh));
  // other.ep may occupy the released descriptor. Force reuse for the client.
  if (other.ep == fd) { int ep = fcntl(other.ep, F_DUPFD_CLOEXEC, 128); assert(ep >= 0); close(other.ep); other.ep = ep; }
  if (fresh[1] == fd) { int p = fresh[0]; fresh[0] = fresh[1]; fresh[1] = p; }
  if (fresh[0] != fd) { assert(dup3(fresh[0], fd, O_CLOEXEC) == fd); close(fresh[0]); fresh[0] = fd; }
  conns[fd].open = 1;
  ep_set(other.ep, EPOLL_CTL_ADD, fd, fd, EPOLLIN);
  res_t ok = {.st = ST_OK, .n = 1}; respond(&t, &stale, &ok, NULL, 0);
  char b; assert(read(fresh[1], &b, 1) < 0 && errno == EAGAIN);
  assert(conns[fd].open); conn_close(&other, fd); close_pending(&other);
  close(other.ep); close(fresh[1]); close(peer);
  puts("ok deferred batch lifetime, forced cross-worker fd reuse, late-result rejection");
}

static pthread_barrier_t reuse_barrier;
static void *reuse_owner(void *arg) {
  int fd = *(int *)arg; th_t owner = {.ep = epoll_create1(EPOLL_CLOEXEC)};
  // Keep the desired client number free even if epoll_create consumed it.
  if (owner.ep == fd) { int ep = fcntl(owner.ep, F_DUPFD_CLOEXEC, 128); assert(ep >= 0); close(owner.ep); owner.ep = ep; }
  pthread_barrier_wait(&reuse_barrier);
  for (int k = 0; k < 256; k++) {
    int s[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, s));
    assert(s[0] == fd); conn_t *c = &conns[fd]; c->open = c->busy = 1;
    c->in = malloc(32); c->inlen = c->incap = 32; memset(c->in, 'a', 32);
    ep_set(owner.ep, EPOLL_CTL_ADD, fd, fd, EPOLLIN);
    char b; assert(read(s[1], &b, 1) < 0 && errno == EAGAIN);
    conn_close(&owner, fd); close_pending(&owner); close(s[1]);
  }
  close(owner.ep); return NULL;
}
static void concurrent_generation(void) {
  int peer, fd = client(&peer);
  q_t stale = {.fd = fd, .gen = conns[fd].gen, .kind = Q_LIKE};
  done(fd, peer); assert(!pthread_barrier_init(&reuse_barrier, NULL, 2));
  pthread_t worker; assert(!pthread_create(&worker, NULL, reuse_owner, &fd));
  pthread_barrier_wait(&reuse_barrier);
  res_t ok = {.st = ST_OK, .n = 1};
  for (int k = 0; k < 4096; k++) respond(&t, &stale, &ok, NULL, 0);
  assert(!pthread_join(worker, NULL)); assert(!pthread_barrier_destroy(&reuse_barrier));
  assert(!conns[fd].open);
  puts("ok concurrent worker owns reused fd while old worker drops late generations");
}

static void halfclose_queries(void) {
  pg_t *p = &t.pg[0]; int dbpeer = wire(p), peer, fd = client(&peer);
  const char req[] = "GET /health HTTP/1.1\r\n\r\nGET /health HTTP/1.1\r\n\r\nGET /health HTTP/1.1\r\n\r\nGET /truncated";
  assert(write(peer, req, sizeof req - 1) == sizeof req - 1); assert(!shutdown(peer, SHUT_WR));
  assert(step() == 1 && conns[fd].busy && conns[fd].inlen);
  assert(step() == 1 && conns[fd].watch == CLIENT_PAUSED); // one lazy EOF pause
  assert(step() == 0); // no repeated busy-input wakeup
  char b[4096]; size_t n = 0;
  for (int k = 0; k < 3; k++) {
    assert(p->n && p->q[p->head].kind == Q_HEALTH);
    health_reply(p, dbpeer); n += drain(peer, b + n, sizeof b - n - 1);
  }
  assert(!conns[fd].busy); assert(step() == 1); assert(!conns[fd].open);
  b[n] = 0; int responses = 0;
  for (char *x = b; (x = strstr(x, "HTTP/1.1 200 ")); x++) responses++;
  assert(responses == 3 && strstr(b, "\"db\":\"ok\""));
  char c; assert(!read(peer, &c, 1)); close(peer); wire_done(p, dbpeer);

  // A completed write remains silent until ReadyForQuery even after SHUT_WR.
  p = &t.pg[3]; dbpeer = wire(p); fd = client(&peer); conns[fd].busy = 1;
  *qpush(p) = (q_t){.fd = fd, .gen = conns[fd].gen, .kind = Q_LIKE, .id = 7}; pg_sync(p);
  assert(!shutdown(peer, SHUT_WR)); assert(step() == 1); assert(conns[fd].open);
  assert(step() == 0); msg(dbpeer, 'C', "INSERT 0 1", 11); pg_read(&t, p);
  assert(p->nheld == 1 && read(peer, &c, 1) < 0 && errno == EAGAIN);
  msg(dbpeer, 'Z', "I", 1); pg_read(&t, p);
  n = drain(peer, b, sizeof b - 1); b[n] = 0; assert(strstr(b, "HTTP/1.1 201 "));
  assert(step() == 1 && !conns[fd].open); assert(!read(peer, &c, 1));
  close(peer); wire_done(p, dbpeer);
  puts("ok half-close preserves three ordered DB reads and a commit-gated write, no EOF spin");
}

static void output_backpressure(void) {
  int peer, fd = client(&peer), sz = 1024;
  assert(!setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz));
  // Start with fragmented input before a fake slow response.
  assert(write(peer, "G", 1) == 1); assert(step() == 1);
  free(conns[fd].in); conns[fd].in = NULL; conns[fd].inlen = conns[fd].incap = 0;
  const size_t len = 180000; char *data = malloc(len), *actual = malloc(len + 4096); memset(data, 'x', len);
  send_output(&t, fd, data, len, NULL, 0, 0); assert(conns[fd].out); client_arm(&t, fd);
  char req[] = "GET /unknown HTTP/1.1\r\n\r\n";
  assert(write(peer, req, sizeof req - 1) == sizeof req - 1); assert(!shutdown(peer, SHUT_WR));
  assert(step() == 0); assert(!conns[fd].in); // later input stays in kernel
  size_t n = 0; int turns = 0;
  while (conns[fd].open && conns[fd].out) {
    n += drain(peer, actual + n, len + 4095 - n); step(); assert(++turns < 1000);
  }
  assert(conns[fd].open); assert(step() == 1); // input resumes only after output drain
  n += drain(peer, actual + n, len + 4095 - n); actual[n] = 0;
  assert(n > len && !memcmp(actual, data, len) && strstr(actual + len, "HTTP/1.1 404 "));
  assert(step() == 1 && !conns[fd].open); char c; assert(!read(peer, &c, 1));
  free(data); free(actual); close(peer);
  puts("ok real epoll output stalls pause input and resume an ordered half-closed pipeline");
}

static void lazy_control_calls(void) {
  int peer, fd = client(&peer); unsigned before = ctl_mods;
  // Immediate replies, fragments, and a complete DB request/response all keep
  // the original level-triggered watch. No epoll_ctl is needed for any of them.
  const char req[] = "GET /unknown HTTP/1.1\r\n\r\n";
  char b[4096];
  assert(write(peer, "G", 1) == 1 && step() == 1);
  assert(conns[fd].inlen == 1 && conns[fd].incap == 1024 && step() == 0);
  assert(write(peer, req + 1, sizeof req - 2) == sizeof req - 2 && step() == 1);
  assert(drain(peer, b, sizeof b) > 0 && !conns[fd].in);
  assert(write(peer, req, sizeof req - 1) == sizeof req - 1 && step() == 1);
  assert(drain(peer, b, sizeof b) > 0);
  pg_t *p = &t.pg[0]; int dbpeer = wire(p);
  const char health[] = "GET /health HTTP/1.1\r\n\r\n";
  assert(write(peer, health, sizeof health - 1) == sizeof health - 1 && step() == 1);
  assert(conns[fd].busy && conns[fd].watch == CLIENT_INPUT && step() == 0);
  health_reply(p, dbpeer); assert(drain(peer, b, sizeof b) > 0 && step() == 0);
  assert(ctl_mods == before && conns[fd].watch == CLIENT_INPUT);
  done(fd, peer); wire_done(p, dbpeer);
  puts("ok real epoll: zero MOD calls for fragments, immediate replies, and normal DB round trip");
}

static void lazy_busy_and_stale_event(void) {
  pg_t *p = &t.pg[0]; int dbpeer = wire(p), peer, fd = client(&peer);
  const char req[] = "GET /health HTTP/1.1\r\n\r\n";
  const char next[] = "GET /unknown HTTP/1.1\r\n\r\n";
  char b[4096]; unsigned before = ctl_mods;
  assert(write(peer, req, sizeof req - 1) == sizeof req - 1 && step() == 1);
  assert(conns[fd].busy && ctl_mods == before);
  assert(write(peer, next, sizeof next - 1) == sizeof next - 1 && step() == 1);
  assert(conns[fd].watch == CLIENT_PAUSED && !conns[fd].in && ctl_mods == before + 1);
  assert(step() == 0); client_arm(&t, fd); assert(ctl_mods == before + 1);
  health_reply(p, dbpeer);
  size_t n = drain(peer, b, sizeof b - 1); b[n] = 0;
  assert(strstr(b, "HTTP/1.1 200 ") && !strstr(b, "404"));
  assert(conns[fd].watch == CLIENT_INPUT && ctl_mods == before + 2);
  assert(step() == 1); n = drain(peer, b, sizeof b - 1); b[n] = 0;
  assert(strstr(b, "HTTP/1.1 404 ") && ctl_mods == before + 2);

  // A PG_FIRST callback can finish the old request before a captured client
  // event is dispatched. The old event may safely read the waiting next request.
  assert(write(peer, req, sizeof req - 1) == sizeof req - 1 && step() == 1);
  assert(write(peer, next, sizeof next - 1) == sizeof next - 1);
  struct epoll_event ev; assert(epoll_wait(t.ep, &ev, 1, 0) == 1);
  health_reply(p, dbpeer);
  client_event(&t, ev.data.u64, ev.events);
  n = drain(peer, b, sizeof b - 1); b[n] = 0;
  char *first = strstr(b, "HTTP/1.1 200 "), *second = strstr(b, "HTTP/1.1 404 ");
  assert(first && second && first < second && ctl_mods == before + 2);
  done(fd, peer); wire_done(p, dbpeer);
  puts("ok lazy busy pause/resume, bounded unread input, PG_FIRST stale-event reply ordering");
}

static void sticky_hup_and_watch_error(void) {
  int peer, fd = client(&peer); conns[fd].busy = 1;
  q_t q = {.fd = fd, .gen = conns[fd].gen, .kind = Q_LIKE, .id = 7};
  close(peer); assert(step() == 1 && conns[fd].watch == CLIENT_PAUSED);
  // MOD may queue one unconditional HUP edge. It must never rearm that edge.
  assert(step() <= 1); assert(step() == 0 && step() == 0 && conns[fd].open);
  res_t ok = {.st = ST_OK, .n = 1}; respond(&t, &q, &ok, NULL, 0);
  assert(!conns[fd].open); close_pending(&t); assert(step() == 0);

  fd = client(&peer); int sz = 1024;
  assert(!setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz));
  char data[20000]; memset(data, 'q', sizeof data);
  send_output(&t, fd, data, sizeof data, NULL, 0, 0); client_arm(&t, fd);
  assert(conns[fd].out && conns[fd].watch == CLIENT_OUTPUT);
  close(peer); assert(step() == 1 && !conns[fd].open && step() == 0);

  fd = client(&peer); conns[fd].busy = 1; fail_mod_fd = fd;
  assert(write(peer, "G", 1) == 1 && step() == 1);
  assert(!conns[fd].open && fcntl(fd, F_GETFD) == -1 && errno == EBADF);
  fail_mod_fd = -1; close(peer);
  puts("ok sticky full HUP while busy/output-blocked never spins; failed watch change closes safely");
}

static void edge_output_short_writes(void) {
  int peer, fd = client(&peer); const size_t len = 4096;
  char data[len], actual[len]; memset(data, 's', len);
  conn_t *c = &conns[fd]; c->out = malloc(len); memcpy(c->out, data, len); c->outlen = len;
  client_arm(&t, fd); assert(c->watch == CLIENT_OUTPUT);
  short_write_fd = fd; interrupt_write = 1;
  assert(step() == 1); // must retry EINTR and drain every successful short write
  short_write_fd = -1;
  assert(!c->out && c->watch == CLIENT_INPUT);
  assert(drain(peer, actual, sizeof actual) == len && !memcmp(actual, data, len));
  assert(step() == 0); done(fd, peer);
  puts("ok edge-triggered output drains real short writes and retries EINTR before waiting");
}

static void edge_output_carried_response(void) {
  int peer, fd = client(&peer); conn_t *c = &conns[fd];
  char first[] = "first response", req[] = "GET /unknown HTTP/1.1\r\n\r\n";
  c->out = strdup(first); c->outlen = sizeof first - 1;
  c->in = strdup(req); c->inlen = c->incap = sizeof req - 1;
  client_arm(&t, fd); interrupt_writev_fd = fd;
  assert(step() == 1 && interrupt_writev_fd == -1);
  assert(c->open && !c->out && !c->in && c->watch == CLIENT_INPUT);
  char actual[4096]; size_t n = drain(peer, actual, sizeof actual - 1); actual[n] = 0;
  assert(!strncmp(actual, first, sizeof first - 1));
  assert(strstr(actual + sizeof first - 1, "HTTP/1.1 404 ") && step() == 0);
  done(fd, peer);
  puts("ok carried response after interrupted writev drains without assuming a new output edge");
}

static void split_case_n(const char *req, size_t len, int valid, int closed) {
  for (size_t split = 0; split <= len; split++) {
    int peer, fd = client(&peer); char *copy = malloc(len); memcpy(copy, req, len);
    process(&t, fd, copy, split);
    if (conns[fd].open && split < len) {
      if (conns[fd].inlen) {
        assert(input_room(&conns[fd], conns[fd].inlen + len - split));
        memcpy(conns[fd].in + conns[fd].inlen, copy + split, len - split);
        conns[fd].inlen += len - split; process(&t, fd, conns[fd].in, conns[fd].inlen);
      } else process(&t, fd, copy + split, len - split);
    }
    char b[4096]; size_t n = drain(peer, b, sizeof b - 1); b[n] = 0;
    if (valid) { assert(strstr(b, "HTTP/1.1 404 ")); assert(conns[fd].open == !closed); }
    else { assert(!conns[fd].open && !n); }
    free(copy); done(fd, peer);
  }
}
static void split_case(const char *req, int valid, int closed) {
  split_case_n(req, strlen(req), valid, closed);
}
static void byte_framing(void) {
  char name[] = "GET /unknown HTTP/1.1\r\nXxName: yes\r\n\r\n";
  size_t name_pos = strlen("GET /unknown HTTP/1.1\r\nX");
  char value[] = "GET /unknown HTTP/1.1\r\nX: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n\r\n";
  size_t value_start = strlen("GET /unknown HTTP/1.1\r\nX: ");
  const size_t offsets[] = {0, 15, 16, 31, 32};
  for (unsigned b = 0; b < 256; b++) {
    // An injected colon ends the name early and is legal value punctuation.
    int token = (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') ||
      (b && strchr("!#$%&'*+-.^_`|~", b));
    name[name_pos] = b; split_case_n(name, sizeof name - 1, token || b == ':', 0);
    for (size_t i = 0; i < sizeof offsets / sizeof *offsets; i++) {
      value[value_start + offsets[i]] = b;
      split_case_n(value, sizeof value - 1, b == 9 || (b >= 32 && b != 127), 0);
      value[value_start + offsets[i]] = 'a';
    }
  }
  split_case("GET /unknown HTTP/1.1\r\nAccept-Encoding: gzip;foo=bar, br;q=0\r\n\r\n", 1, 0);
  split_case("GET /unknown HTTP/1.1\r\nAccept-Encoding: gzip;q=0\r\nAccept-Encoding: *;q=1\r\n\r\n", 1, 0);
  puts("ok binary HTTP corpus at every split: all 256 name/value bytes and five SIMD-boundary positions");
}
static void framing(void) {
  split_case("GET /unknown HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\nContent-Length: 02\r\nX!#$%&'*+-.^_`|~: yes\r\n\r\n{}", 1, 0);
  split_case("GET /unknown HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", 1, 0);
  split_case("GET /unknown HTTP/1.1\r\nConnection: keep-alive, CLOSE\r\nConnection: keep-alive\r\n\r\n", 1, 1);
  const char *bad[] = {
    "GET /unknown HTTP/1.2\r\n\r\n", "GET /unknown HTTP/1.1\nX: y\r\n\r\n",
    "GET /unknown HTTP/1.1\r\nNoColon\r\n\r\n", "GET /unknown HTTP/1.1\r\nX: a\rb\r\n\r\n",
    "GET /unknown HTTP/1.1\r\nX: a\nb\r\n\r\n", "GET /unknown HTTP/1.1\r\nBad Header: x\r\n\r\n",
    "GET /unknown HTTP/1.1\r\nContent-Length:\r\n\r\n", "GET /unknown HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
    "GET /unknown HTTP/1.1\r\nContent-Length: 99999999999999999999999\r\n\r\n",
    "GET /unknown HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 0\r\n\r\n{}",
    "GET /unknown HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
    "GET /unknown HTTP/1.1\r\nAuthorization: x\r\nAuthorization: y\r\n\r\n"
  };
  for (size_t k = 0; k < sizeof bad / sizeof *bad; k++) split_case(bad[k], 0, 1);
  // Combined header/body cap, including a complete oversized header in one call.
  for (int excess = 0; excess <= 1; excess++) {
    int peer, fd = client(&peer); size_t len = MAXREQ + excess;
    char *req = malloc(len); const char *head = "GET /unknown HTTP/1.1\r\nX: "; size_t hn = strlen(head);
    memcpy(req, head, hn); memset(req + hn, 'a', len - hn - 4); memcpy(req + len - 4, "\r\n\r\n", 4);
    process(&t, fd, req, len); assert(conns[fd].open == !excess); free(req); done(fd, peer);
  }
  int peer, fd = client(&peer); char capreq[] = "GET /unknown HTTP/1.1\r\nContent-Length: 65536\r\n\r\n";
  process(&t, fd, capreq, sizeof capreq - 1); assert(!conns[fd].open); done(fd, peer);
  puts("ok HTTP framing corpus at every split, full tchar names, exact combined request cap");
}

static void negotiation_and_expect(void) {
  const struct { const char *s; int allowed; } cases[] = {
    {"gzip", 1}, {"GZip", 1}, {"xgzip", 0}, {"gzip;q=0", 0}, {"gzip;q=0.001", 1},
    {"gzip;foo=bar, br;q=0", 1}, {"br;q=0, gzip; q=1.000", 1}, {"gzip;q=0, *;q=1", 0},
    {"*;q=0.5", 1}, {"*;q=0", 0}, {"gzip;q=0.0, gzip", 0}, {"gzip;q=bogus", 0},
    {"gzip;q=1.001", 0}, {"gzip;q=0.0001", 0}, {"gzip;q=0.5;q=1", 0}, {"br", 0}
  };
  for (size_t k = 0; k < sizeof cases / sizeof *cases; k++)
    assert(accepts_gzip(cases[k].s, cases[k].s + strlen(cases[k].s)) == cases[k].allowed);
  const char *wild = "*", *deny = "gzip;q=0";
  assert(!encoding_gzip(encoding_flags(wild, wild + strlen(wild)) | encoding_flags(deny, deny + strlen(deny))));
  int peer, fd = client(&peer);
  char req[] = "POST /posts HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 2\r\n\r\n{";
  assert(write(peer, req, sizeof req - 1) == sizeof req - 1); assert(step() == 1);
  char b[4096]; size_t n = drain(peer, b, sizeof b - 1); b[n] = 0;
  assert(!strcmp(b, "HTTP/1.1 100 Continue\r\n\r\n"));
  assert(write(peer, "}", 1) == 1); assert(step() == 1);
  n = drain(peer, b, sizeof b - 1); b[n] = 0;
  assert(strstr(b, "HTTP/1.1 401 ") && !strstr(b, "100 Continue")); done(fd, peer);
  puts("ok gzip token/quality/wildcard policy and partial-body 100 Continue emitted once");
}

int main(void) {
  signal(SIGPIPE, SIG_IGN); maxfd = 8192; conns = calloc(maxfd, sizeof *conns);
  arena = malloc(OBUF); zbuf = malloc(OBUF); now_s = time(NULL); now_ns = mono_ns();
  t.ep = epoll_create1(EPOLL_CLOEXEC); t.npg = 4; t.pg = calloc(t.npg, sizeof *t.pg);
  for (int k = 0; k < t.npg; k++) t.pg[k].sock = -1;
  deferred_batch(); concurrent_generation(); lazy_control_calls(); lazy_busy_and_stale_event();
  halfclose_queries(); output_backpressure(); sticky_hup_and_watch_error(); edge_output_short_writes(); edge_output_carried_response();
  framing(); byte_framing(); negotiation_and_expect();
  close_pending(&t); close(t.ep); free(t.pg); free(arena); free(zbuf); free(conns);
  puts("all focused round3-a transport regressions passed");
}
