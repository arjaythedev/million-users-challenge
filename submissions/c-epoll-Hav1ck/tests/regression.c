// Focused, deterministic wire/socket tests. No database and no load generator.
#define main code_b_server_main
#include "../server.c"
#undef main

static int pairs[128][2], npairs;
static th_t t;

static int client(void) {
  assert(npairs < 128);
  int *s = pairs[npairs++];
  assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, s));
  assert(s[0] < maxfd && s[1] < maxfd);
  conns[s[0]].open = 1;
  ep_set(t.ep, EPOLL_CTL_ADD, s[0], s[0], EPOLLIN);
  return s[0];
}
static int peer(int fd) {
  for (int k = 0; k < npairs; k++) if (pairs[k][0] == fd) return pairs[k][1];
  abort();
}
static void no_output(int fd) {
  char b; assert(read(peer(fd), &b, 1) == -1 && errno == EAGAIN);
}
static void status_is(int fd, int status) {
  char b[4096], prefix[32]; ssize_t n = read(peer(fd), b, sizeof b - 1);
  assert(n > 0); b[n] = 0;
  snprintf(prefix, sizeof prefix, "HTTP/1.1 %d ", status);
  assert(!strncmp(b, prefix, strlen(prefix)));
}
static int wire(pg_t *p) {
  int s[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, s));
  p->sock = s[0]; p->icap = 65536; p->ib = malloc(p->icap);
  ep_set(t.ep, EPOLL_CTL_ADD, p->sock, TAG_PG | (p - t.pg), EPOLLIN);
  return s[1];
}
static void msg(int fd, char ty, const char *payload, size_t n) {
  char b[1024]; assert(n + 5 <= sizeof b); b[0] = ty; w32(b + 1, n + 4);
  memcpy(b + 5, payload, n); assert(write(fd, b, n + 5) == (ssize_t)n + 5);
}
static void command(int fd) { msg(fd, 'C', "INSERT 0 1", 11); }
static void ready(int fd) { msg(fd, 'Z', "I", 1); }
static void error(int fd) { static const char e[] = "C23503\0Mforeign key violation\0"; msg(fd, 'E', e, sizeof e); }
static q_t like(int fd, int64_t id) {
  conns[fd].busy = 1;
  return (q_t){.fd = fd, .gen = conns[fd].gen, .kind = Q_LIKE, .id = id};
}
static void cleanup_pg(pg_t *p, int other) {
  if (p->sock >= 0) close(p->sock);
  close(other); free(p->ib); free(p->ob); free(p->q); memset(p, 0, sizeof *p); p->sock = -1;
}

static void held_and_failed_groups(void) {
  pg_t *p = &t.pg[3]; int other = wire(p), fd = client();
  *qpush(p) = like(fd, 10); pg_sync(p);
  command(other); pg_read(&t, p);
  assert(p->nheld == 1 && conns[fd].busy); no_output(fd);
  ready(other); pg_read(&t, p);
  assert(p->nheld == 0 && !conns[fd].busy); status_is(fd, 201);
  cleanup_pg(p, other);

  other = wire(p); fd = client(); *qpush(p) = like(fd, 11); pg_sync(p);
  command(other); pg_read(&t, p); no_output(fd);
  error(other); ready(other); pg_read(&t, p);
  status_is(fd, 500); assert(!p->n && !p->nheld); cleanup_pg(p, other);

  // One succeeded, one failed, one was skipped. Only the failed request gets 404;
  // the rolled-back success and skipped request each get a separate retry/Sync.
  other = wire(p); int good = client(), bad = client(), skipped = client();
  *qpush(p) = like(good, 20); *qpush(p) = like(bad, 999999); *qpush(p) = like(skipped, 21); pg_sync(p);
  command(other); error(other); pg_read(&t, p);
  assert(p->nheld == 3); no_output(good); no_output(bad); no_output(skipped);
  ready(other); pg_read(&t, p);
  status_is(bad, 404); no_output(good); no_output(skipped);
  assert(p->n == 4 && p->q[p->head].fd == good);
  assert(p->q[(p->head + 1) & (p->cap - 1)].kind == Q_SYNC);
  assert(p->q[(p->head + 2) & (p->cap - 1)].fd == skipped);
  assert(p->q[(p->head + 3) & (p->cap - 1)].kind == Q_SYNC);
  command(other); ready(other); command(other); ready(other); pg_read(&t, p);
  status_is(good, 201); status_is(skipped, 201); assert(!p->n); cleanup_pg(p, other);
  puts("ok held writes, commit failure, isolated failed-group retries");
}


// Round 2: a CommandComplete is never enough to acknowledge a write. Only a
// correctly framed idle ReadyForQuery can release it, even across fragmented I/O.
static void commit_boundaries(void) {
  static const struct { const char *body; size_t n; } invalid[] = {
    {"", 0}, {"II", 2}, {"T", 1}, {"E", 1}, {"X", 1}
  };
  pg_t *p = &t.pg[3];
  for (size_t k = 0; k < sizeof invalid / sizeof *invalid; k++) {
    int other = wire(p), held = client(), queued = client();
    *qpush(p) = like(held, 50); pg_sync(p);
    *qpush(p) = like(queued, 51); pg_sync(p);
    command(other); pg_read(&t, p);
    assert(p->nheld == 1); no_output(held); no_output(queued);
    msg(other, 'Z', invalid[k].body, invalid[k].n); pg_read(&t, p);
    status_is(held, 500); status_is(queued, 500);
    assert(p->sock == -1 && !p->n && !p->nheld && !p->olen);
    // No uncertain write is replayed on any other lane.
    for (int i = 0; i < t.npg; i++) assert(!t.pg[i].n);
    cleanup_pg(p, other);
  }
  for (int phase = 0; phase < 3; phase++) {
    int other = wire(p), fd = client();
    *qpush(p) = like(fd, 52); pg_sync(p);
    if (phase) { command(other); pg_read(&t, p); no_output(fd); }
    if (phase == 2) { // Incomplete Z body followed by disconnect is still uncertain.
      const char partial[] = {'Z', 0, 0, 0, 5};
      assert(write(other, partial, sizeof partial) == sizeof partial);
      pg_read(&t, p); no_output(fd);
    }
    assert(!shutdown(other, SHUT_WR)); pg_read(&t, p); status_is(fd, 500);
    assert(p->sock == -1 && !p->n && !p->nheld && !p->olen);
    cleanup_pg(p, other);
  }
  // INSERT with real RETURNING fields also remains uncertain after C + EOF.
  int other = wire(p), fd = client(); conns[fd].busy = 1;
  *qpush(p) = (q_t){.fd = fd, .gen = conns[fd].gen, .kind = Q_INS,
                   .extra = strdup("bodyalice"), .blen = 4, .ulen = 5};
  pg_sync(p);
  char row[26]; w16(row, 2); w32(row + 2, 8); w32(row + 14, 8);
  uint64_t id = __builtin_bswap64(123), ts = 0;
  memcpy(row + 6, &id, 8); memcpy(row + 18, &ts, 8);
  msg(other, 'D', row, sizeof row); command(other); pg_read(&t, p); no_output(fd);
  assert(p->nheld == 1 && p->hres[0].n == 1 && p->hres[0].id == 123);
  assert(!shutdown(other, SHUT_WR)); pg_read(&t, p); status_is(fd, 500);
  for (int i = 0; i < t.npg; i++) assert(!t.pg[i].n);
  cleanup_pg(p, other);

  other = wire(p); fd = client();
  *qpush(p) = like(fd, 53); pg_sync(p); pg_sync(p); // empty Sync behind the real group
  command(other); pg_read(&t, p); no_output(fd);
  const char z[] = {'Z', 0, 0, 0, 5, 'I'};
  assert(write(other, z, 5) == 5); pg_read(&t, p); no_output(fd);
  assert(write(other, z + 5, 1) == 1); pg_read(&t, p); status_is(fd, 201);
  assert(p->n == 1 && !p->nheld); ready(other); pg_read(&t, p); no_output(fd);
  assert(!p->n && !p->cfail && !p->gfail); cleanup_pg(p, other);

  // Empty Syncs recover cleanly after a rolled-back group, and cannot release
  // an isolated retry belonging to a later Sync.
  other = wire(p); int good = client(), bad = client();
  *qpush(p) = like(good, 54); *qpush(p) = like(bad, 999999); pg_sync(p); pg_sync(p);
  command(other); error(other); pg_read(&t, p); no_output(good); no_output(bad);
  ready(other); pg_read(&t, p); status_is(bad, 404); no_output(good);
  assert(p->n == 3 && p->q[p->head].kind == Q_SYNC);
  ready(other); pg_read(&t, p); no_output(good); assert(p->n == 2);
  command(other); pg_read(&t, p); no_output(good);
  ready(other); pg_read(&t, p); status_is(good, 201); assert(!p->n);
  cleanup_pg(p, other);
  puts("ok malformed/non-idle/fragmented ReadyForQuery, disconnect, empty Sync ordering");
}

static void failure_and_generation(void) {
  pg_t *p = &t.pg[0], *otherp = &t.pg[1]; int other = wire(p), alt = wire(otherp);
  int readfd = client(), writefd = client(), heldfd = client();
  conns[readfd].busy = 1;
  *qpush(p) = (q_t){.fd = readfd, .gen = conns[readfd].gen, .kind = Q_HEALTH};
  *qpush(p) = like(writefd, 31); pg_sync(p);
  p->held[0] = like(heldfd, 32); p->hres[0] = (res_t){.st = ST_OK, .n = 1}; p->nheld = 1;
  p->olen = 77; p->ooff = 20; p->flush_at = 99; p->dirty = p->wout = 1;
  pg_fail(&t, p, "test disconnect");
  status_is(writefd, 500); status_is(heldfd, 500); no_output(readfd);
  assert(p->sock == -1 && !p->n && !p->nheld && !p->olen && !p->ooff && !p->flush_at);
  assert(otherp->n == 1 && otherp->q[otherp->head].kind == Q_HEALTH && otherp->q[otherp->head].tries == 1);
  assert(!otherp->wout); pg_sync(otherp);
  msg(alt, 'C', "SELECT 1", 9); ready(alt); pg_read(&t, otherp); status_is(readfd, 200);
  cleanup_pg(p, other); cleanup_pg(otherp, alt);
  other = wire(p); readfd = client(); conns[readfd].busy = 1;
  *qpush(p) = (q_t){.fd = readfd, .gen = conns[readfd].gen, .kind = Q_HEALTH}; pg_sync(p);
  pg_fail(&t, p, "test no alternative connection"); status_is(readfd, 503); cleanup_pg(p, other);

  // A descriptor's previous occupant must never get a late success or retry.
  int fd = client(); q_t stale = like(fd, 33); conns[fd].gen++;
  res_t ok = {.st = ST_OK, .n = 1}; respond(&t, &stale, &ok, NULL, 0); no_output(fd);
  puts("ok lost connection resets, read-only retry, stale generation rejection");
}

static void selection_and_bounds(void) {
  t.pick_next[0] = t.pick_next[1] = 0;
  for (int k = 0; k < 4; k++) t.pg[k].sock = 1000 + k; // selectors perform no I/O
  assert(pick(&t, 0) == &t.pg[0]); assert(pick(&t, 0) == &t.pg[1]); assert(pick(&t, 0) == &t.pg[2]);
  t.pg[1].unsynced = 1; t.pg[1].inbatch = 1; assert(pick(&t, 0) == &t.pg[1]);
  t.pg[1].n = 2 * PACK; assert(pick(&t, 0) != &t.pg[1]); t.pg[1].n = 0;
  t.pg[1].wout = 1; assert(pick(&t, 0) != &t.pg[1]);
  for (int k = 0; k < 3; k++) t.pg[k].n = PG_ADMIT_LIMIT;
  assert(!pick(&t, 0)); assert(pick(&t, 1) == &t.pg[3]);
  for (int k = 0; k < 4; k++) { memset(&t.pg[k], 0, sizeof t.pg[k]); t.pg[k].sock = -1; }
  int other = wire(&t.pg[0]), fd = client();
  for (int k = 0; k < PACK; k++) assert(enqueue(&t, fd, Q_HEALTH, 0, 0, 0, NULL, 0, NULL, 0));
  assert(t.pg[0].n == PACK + 1 && !t.pg[0].unsynced && t.pg[0].flush_at == 0);
  cleanup_pg(&t.pg[0], other);
  other = wire(&t.pg[0]);
  assert(enqueue(&t, fd, Q_HEALTH, 0, 0, 0, NULL, 0, NULL, 0));
  int64_t deadline = t.pg[0].flush_at; now_ns++;
  assert(enqueue(&t, fd, Q_HEALTH, 0, 0, 0, NULL, 0, NULL, 0)); assert(t.pg[0].flush_at == deadline);
  int admitted = 2;
  while (enqueue(&t, fd, Q_HEALTH, 0, 0, 0, NULL, 0, NULL, 0)) { assert(++admitted < PG_MAX_QUEUE); }
  assert(t.pg[0].n + 2 >= PG_ADMIT_LIMIT && t.pg[0].n < PG_ADMIT_LIMIT && t.pg[0].cap <= PG_MAX_QUEUE);
  cleanup_pg(&t.pg[0], other);
  other = wire(&t.pg[3]);
  for (int k = 0; k < GROUP; k++) assert(enqueue(&t, fd, Q_LIKE, 0, k + 1, 1, NULL, 0, NULL, 0));
  assert(t.pg[3].n == GROUP + 1 && !t.pg[3].unsynced && t.pg[3].flush_at == 0);
  cleanup_pg(&t.pg[3], other);
  puts("ok rotated selection, blocked/full queue backpressure, hard batch limits");
}

static size_t drain(int fd, char *out, size_t used, size_t cap) {
  while (used < cap) {
    ssize_t n = read(fd, out + used, cap - used);
    if (n < 0) { assert(errno == EAGAIN); break; }
    if (!n) break;
    used += n;
  }
  return used;
}
static void partial_postgres(void) {
  pg_t *p = &t.pg[0]; int other = wire(p), sz = 1024;
  assert(!setsockopt(p->sock, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz));
  size_t initial = 200000, extra = 300000, total = initial + extra; char *expected = malloc(total), *actual = malloc(total);
  for (size_t k = 0; k < total; k++) expected[k] = k * 13;
  memcpy(ob_room(p, initial), expected, initial); pg_flush(&t, p);
  assert(p->wout && p->ooff > 0 && p->ooff < p->olen);
  // Appending after the short write forces the deferred compaction path.
  assert(p->olen + extra > p->ocap);
  memcpy(ob_room(p, extra), expected + initial, extra); assert(!p->ooff);
  size_t n = 0; int passes = 0;
  while (p->olen) { n = drain(other, actual, n, total); pg_flush(&t, p); assert(++passes < 10000); }
  n = drain(other, actual, n, total);
  assert(n == total && !memcmp(expected, actual, total) && !p->wout && !p->ooff);
  free(expected); free(actual); cleanup_pg(p, other);
  puts("ok Postgres partial-write offset preserves exact bytes");
}

static void partial_http_and_continue(void) {
  int fd = client(), sz = 1024; assert(!setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz));
  size_t total = 180000; char *head = malloc(total), *actual = malloc(total + 4096);
  memset(head, 'x', total);
  send_output(&t, fd, head, total, "TAIL", 4, 0); assert(conns[fd].out);
  char next[] = "GET /unknown HTTP/1.1\r\nHost: x\r\n\r\n";
  process(&t, fd, next, sizeof next - 1); assert(conns[fd].inlen == sizeof next - 1);
  size_t n = 0; int passes = 0;
  while (conns[fd].out) { n = drain(peer(fd), actual, n, total + 4095); on_writable(&t, fd); assert(++passes < 10000); }
  n = drain(peer(fd), actual, n, total + 4095); actual[n] = 0;
  assert(n > total + 4 && !memcmp(actual, head, total) && !memcmp(actual + total, "TAILHTTP/1.1 404 ", 17));
  assert(!conns[fd].inlen); free(head); free(actual);

  // Fill the output socket first so 100 Continue itself must survive EAGAIN.
  fd = client(); assert(!setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz));
  char fill[4096]; memset(fill, 'q', sizeof fill); size_t filled = 0;
  for (;;) { ssize_t w = write(fd, fill, sizeof fill); if (w < 0) { assert(errno == EAGAIN); break; } filled += w; }
  char req[] = "POST /posts HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 2\r\n\r\n";
  process(&t, fd, req, sizeof req - 1); assert(conns[fd].continued && conns[fd].out);
  char *buf = malloc(filled + 4096); n = drain(peer(fd), buf, 0, filled + 4095); assert(n == filled);
  on_writable(&t, fd); n = drain(peer(fd), buf, 0, filled + 4095); buf[n] = 0;
  assert(!strcmp(buf, "HTTP/1.1 100 Continue\r\n\r\n")); assert(!conns[fd].out);
  no_output(fd); // processing carried headers did not send another 100
  char body[] = "{}"; assert(write(peer(fd), body, 2) == 2); on_readable(&t, fd);
  status_is(fd, 401); assert(!conns[fd].continued); free(buf);
  puts("ok HTTP partial output/pipeline ordering and EAGAIN on 100 Continue");
}

static void bounded_http_input(void) {
  int fd = client(); conns[fd].busy = 1;
  char req[] = "GET /unknown HTTP/1.1\r\nHost: x\r\n\r\n";
  assert(write(peer(fd), req, sizeof req - 1) == sizeof req - 1);
  on_readable(&t, fd);  // a stale event while busy cannot consume more input
  assert(!conns[fd].in && !conns[fd].incap && conns[fd].open);
  q_t q = {.fd = fd, .gen = conns[fd].gen, .kind = Q_LIKE, .id = 7};
  res_t ok = {.st = ST_OK, .n = 1}; respond(&t, &q, &ok, NULL, 0); status_is(fd, 201);
  on_readable(&t, fd); status_is(fd, 404);
  assert(!conns[fd].in && !conns[fd].busy);
  fd = client(); assert(write(peer(fd), "G", 1) == 1); on_readable(&t, fd);
  assert(conns[fd].inlen == 1 && conns[fd].incap == 1024);
  puts("ok paused busy input, ordered resume, 1 KiB fragmented-input allocation");
}

int main(void) {
  signal(SIGPIPE, SIG_IGN); maxfd = 8192; conns = calloc(maxfd, sizeof *conns);
  arena = malloc(OBUF); zbuf = malloc(OBUF); now_s = time(NULL); now_ns = mono_ns();
  t.ep = epoll_create1(EPOLL_CLOEXEC); t.npg = 4; t.pg = calloc(t.npg, sizeof *t.pg);
  for (int k = 0; k < t.npg; k++) t.pg[k].sock = -1;
  commit_boundaries(); held_and_failed_groups(); failure_and_generation(); selection_and_bounds();
  partial_postgres(); partial_http_and_continue(); bounded_http_input();
  for (int k = 0; k < npairs; k++) { if (conns[pairs[k][0]].open) conn_close(&t, pairs[k][0]); close(pairs[k][1]); }
  close_pending(&t);
  close(t.ep); free(t.pg); free(arena); free(zbuf); free(conns);
  puts("all focused inherited regressions passed");
}
