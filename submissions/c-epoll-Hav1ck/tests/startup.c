// Fake PostgreSQL peers only: no live database, queries, cluster, or load.
#define main round2_server_main
#include "../server.c"
#undef main

typedef struct { int listener, stage; const char *body; uint32_t n; } peer_t;
static void fake_msg(int fd, char ty, const char *body, uint32_t n) {
  char head[5] = {ty}; w32(head + 1, n + 4);
  assert(!io_n(fd, head, 5, 1)); assert(!io_n(fd, (char *)body, n, 1));
}
static void *fake_pg(void *arg) {
  peer_t *s = arg; int fd = accept(s->listener, NULL, NULL); assert(fd >= 0);
  struct timeval timeout = {2, 0};
  assert(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout));
  char m[16384], head[4]; assert(!io_n(fd, head, 4, 0));
  uint32_t n = be32(head); assert(n >= 8 && n <= sizeof m);
  assert(!io_n(fd, m, n - 4, 0));
  fake_msg(fd, 'R', "\0\0\0\0", 4);
  fake_msg(fd, 'Z', s->stage ? "I" : s->body, s->stage ? 1 : s->n);
  if (s->stage || (s->n == 1 && s->body[0] == 'I')) {
    int ty;
    do { ty = rd_msg(fd, m, sizeof m, &n); assert(ty == 'P' || ty == 'S'); } while (ty != 'S');
    for (int k = 0; k < 5; k++) fake_msg(fd, '1', "", 0);
    fake_msg(fd, 'Z', s->body, s->n);
  }
  close(fd); return NULL;
}
int main(void) {
  signal(SIGPIPE, SIG_IGN);
  static const struct { const char *body; uint32_t n; } cases[] = {
    {"", 0}, {"II", 2}, {"T", 1}, {"E", 1}, {"X", 1}, {"I", 1}
  };
  for (int stage = 0; stage < 2; stage++) for (unsigned k = 0; k < sizeof cases / sizeof *cases; k++) {
    peer_t s = {.listener = socket(AF_INET, SOCK_STREAM, 0), .stage = stage,
                .body = cases[k].body, .n = cases[k].n};
    assert(s.listener >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(s.listener, (void *)&address, sizeof address)); assert(!listen(s.listener, 1));
    socklen_t size = sizeof address; assert(!getsockname(s.listener, (void *)&address, &size));
    strcpy(db.host, "127.0.0.1"); snprintf(db.port, sizeof db.port, "%u", ntohs(address.sin_port));
    strcpy(db.user, "fake-test"); strcpy(db.db, "fake-test");
    pg_t pg = {.sock = -1}; th_t th = {.ep = epoll_create1(EPOLL_CLOEXEC), .npg = 1, .pg = &pg};
    pthread_t thread; assert(!pthread_create(&thread, NULL, fake_pg, &s));
    assert(pg_connect(&th, 0) == (k == 5));
    assert(!pthread_join(thread, NULL));
    if (pg.sock >= 0) close(pg.sock);
    free(pg.ib); close(th.ep); close(s.listener);
  }
  puts("ok startup and prepare reject malformed/non-idle ReadyForQuery (12 fake handshakes)");
}
