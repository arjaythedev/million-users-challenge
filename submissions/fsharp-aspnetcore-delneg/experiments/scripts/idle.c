// idle: holds N keep-alive connections to a server (epoll, one thread), for measuring the cost of idle
// connections. Each connection sends one GET when it opens and checks for a 200; then it stays idle. With -k S,
// every connection repeats its GET every S seconds (spread out), so idle connections are checked to still work.
//   idle -n 200000 -ips 2-9 -ports 80,81 -rate 5000 [-k 90] [-path /health]
// Connections go round-robin over 127.0.0.<ips> x ports (~64k source ports per destination). Prints a line every 5 s.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct { int fd; uint32_t dst; uint16_t port; uint8_t st; uint8_t got; double next; } conn_t; // st 0 none,1 connecting,2 sent,3 idle
static conn_t *cs; static long n = 100000, rate = 5000; static int kint = 0;
static char req[256]; static int reqlen;
static long nopen, nok, nfail, nreq, nresp;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void sendreq(int ep, conn_t *c) {
  ssize_t w = send(c->fd, req, reqlen, MSG_NOSIGNAL);
  if (w != reqlen) { nfail++; close(c->fd); c->st = 0; nopen--; return; }
  c->st = 2; c->got = 0; nreq++;
}

int main(int argc, char **argv) {
  int iplo = 2, iphi = 9; char ports[64] = "80,81"; const char *path = "/health";
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "-n")) n = atol(argv[i + 1]);
    else if (!strcmp(argv[i], "-rate")) rate = atol(argv[i + 1]);
    else if (!strcmp(argv[i], "-k")) kint = atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "-ips")) sscanf(argv[i + 1], "%d-%d", &iplo, &iphi);
    else if (!strcmp(argv[i], "-ports")) snprintf(ports, sizeof ports, "%s", argv[i + 1]);
    else if (!strcmp(argv[i], "-path")) path = argv[i + 1];
  }
  int pl[8], np = 0; for (char *p = strtok(ports, ","); p && np < 8; p = strtok(NULL, ",")) pl[np++] = atoi(p);
  reqlen = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: x\r\nUser-Agent: idle\r\nAccept-Encoding: gzip\r\n\r\n", path);
  struct rlimit rl = {n + 1000, n + 1000}; setrlimit(RLIMIT_NOFILE, &rl);
  signal(SIGPIPE, SIG_IGN);
  cs = calloc(n, sizeof(conn_t));
  int ep = epoll_create1(0);
  struct epoll_event evs[4096];
  double t0 = now(), lastp = t0; long started = 0; int nips = iphi - iplo + 1;
  char buf[65536];
  for (;;) {
    double t = now();
    long want = (long)((t - t0) * rate); if (want > n) want = n;
    while (started < want) {
      conn_t *c = &cs[started];
      long k = started++;
      c->dst = htonl((127u << 24) | (uint32_t)(iplo + k % nips));
      c->port = pl[(k / nips) % np];
      int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
      if (fd < 0) { nfail++; continue; }
      int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, 4);
      struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(c->port), .sin_addr.s_addr = c->dst};
      int r = connect(fd, (struct sockaddr *)&a, sizeof a);
      if (r < 0 && errno != EINPROGRESS) { nfail++; close(fd); continue; }
      c->fd = fd; c->st = 1; nopen++;
      struct epoll_event e = {.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP, .data.u64 = (uint64_t)k};
      epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e);
    }
    int m = epoll_wait(ep, evs, 4096, 10);
    for (int i = 0; i < m; i++) {
      conn_t *c = &cs[evs[i].data.u64];
      if (c->st == 0) continue;
      uint32_t ev = evs[i].events;
      if (c->st == 1 && (ev & EPOLLOUT)) {
        int err = 0; socklen_t el = 4; getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) { nfail++; close(c->fd); c->st = 0; nopen--; continue; }
        struct epoll_event e = {.events = EPOLLIN | EPOLLRDHUP, .data.u64 = evs[i].data.u64};
        epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &e);
        sendreq(ep, c);
        if (c->st == 0) continue;
      }
      if (ev & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
        ssize_t r = recv(c->fd, buf, sizeof buf, 0);
        if (r <= 0) { if (r == 0 || errno != EAGAIN) { nfail++; close(c->fd); c->st = 0; nopen--; } continue; }
        if (c->st == 2 && !c->got) {
          c->got = 1; nresp++;
          if (r >= 12 && !memcmp(buf, "HTTP/1.1 200", 12)) nok++; else nfail++;
          c->st = 3;
          if (kint) c->next = t + kint * (0.5 + (double)(evs[i].data.u64 % 1000) / 1000.0);
        }
      }
    }
    if (kint) {
      static long scan = 0; long per = n / 100 + 1; // ~1% of the table per pass
      for (long j = 0; j < per; j++, scan = (scan + 1) % n) {
        conn_t *c = &cs[scan];
        if (c->st == 3 && c->next > 0 && t >= c->next) { sendreq(ep, c); c->next = t + kint; }
      }
    }
    if (t - lastp >= 5) {
      lastp = t;
      printf("t=%.0fs open=%ld started=%ld req=%ld resp=%ld ok=%ld fail=%ld\n", t - t0, nopen, started, nreq, nresp, nok, nfail);
      fflush(stdout);
    }
  }
}
