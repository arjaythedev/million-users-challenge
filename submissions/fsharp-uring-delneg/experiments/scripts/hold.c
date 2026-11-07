// hold: N keep-alive connections from one epoll thread, each sending a GET every ~K seconds (spread out), with a
// latency histogram per 10 s window and in total. For testing a server at the grader's connection counts (1M+),
// which mload (a goroutine per user) can't reach on one box.
//   hold -n 1000000 -ips 2-9 -ports 80,81 -rate 3300 -k 60 -secs 900 [-feedpct 50] [-maxid 500000] [-to 10]
// Requests alternate GET /feed and GET /posts/<random id> (feedpct % feeds). A request unanswered after -to seconds
// counts as a timeout (the connection is closed and reopened). Prints a line every 10 s and a total at the end.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
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

typedef struct { int fd; uint32_t dst; uint16_t port; uint8_t st; double sent, next; } conn_t; // st 0 none 1 connecting 2 waiting 3 idle
static conn_t *cs; static long n = 100000, rate = 3300; static double kint = 60, tmo = 10; static int feedpct = 50; static long maxid = 500000;
static int iplo = 2, iphi = 9, pl[8], np = 0;
#define NB 64
static long hwin[NB], htot[NB]; static long wreq, wresp, wfail, wto, treq, tresp, tfail, tto, nopen, nconn;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static int bucket(double ms) { // ~19% steps from 0.25 ms
  int b = 0; double e = 0.25; while (ms > e && b < NB - 1) { e *= 1.19; b++; } return b;
}
static double bupper(int b) { double e = 0.25; for (int i = 0; i < b; i++) e *= 1.19; return e; }
static double pct(long *h, double p) {
  long tot = 0; for (int i = 0; i < NB; i++) tot += h[i]; if (!tot) return 0;
  long want = (long)(tot * p + 0.999999), acc = 0; for (int i = 0; i < NB; i++) { acc += h[i]; if (acc >= want) return bupper(i); } return bupper(NB - 1);
}
static uint64_t rs = 88172645463325252ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

static void openc(int ep, long k) {
  conn_t *c = &cs[k]; int nips = iphi - iplo + 1;
  c->dst = htonl((127u << 24) | (uint32_t)(iplo + k % nips)); c->port = pl[(k / nips) % np];
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (fd < 0) { c->st = 0; c->next = now() + 1; return; }
  int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, 4);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(c->port), .sin_addr.s_addr = c->dst};
  int r = connect(fd, (struct sockaddr *)&a, sizeof a);
  if (r < 0 && errno != EINPROGRESS) { close(fd); c->st = 0; c->next = now() + 1; return; }
  c->fd = fd; c->st = 1; nopen++; nconn++;
  struct epoll_event e = {.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP, .data.u64 = (uint64_t)k};
  epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e);
}
static void closec(conn_t *c) { if (c->st) { close(c->fd); nopen--; } c->st = 0; c->next = now() + 0.5; }
static void sendreq(conn_t *c) {
  char req[256]; int len;
  if ((long)(rnd() % 100) < feedpct) len = snprintf(req, sizeof req, "GET /feed HTTP/1.1\r\nHost: x\r\nUser-Agent: hold\r\nAccept-Encoding: gzip\r\n\r\n");
  else len = snprintf(req, sizeof req, "GET /posts/%ld HTTP/1.1\r\nHost: x\r\nUser-Agent: hold\r\nAccept-Encoding: gzip\r\n\r\n", 1 + (long)(rnd() % maxid));
  ssize_t w = send(c->fd, req, len, MSG_NOSIGNAL);
  if (w != len) { wfail++; tfail++; closec(c); return; }
  c->st = 2; c->sent = now(); wreq++; treq++;
}

int main(int argc, char **argv) {
  char ports[64] = "80,81"; double secs = 600, warm = 0;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "-n")) n = atol(argv[i + 1]);
    else if (!strcmp(argv[i], "-rate")) rate = atol(argv[i + 1]);
    else if (!strcmp(argv[i], "-k")) kint = atof(argv[i + 1]);
    else if (!strcmp(argv[i], "-to")) tmo = atof(argv[i + 1]);
    else if (!strcmp(argv[i], "-secs")) secs = atof(argv[i + 1]);
    else if (!strcmp(argv[i], "-warm")) warm = atof(argv[i + 1]);
    else if (!strcmp(argv[i], "-feedpct")) feedpct = atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "-maxid")) maxid = atol(argv[i + 1]);
    else if (!strcmp(argv[i], "-ips")) sscanf(argv[i + 1], "%d-%d", &iplo, &iphi);
    else if (!strcmp(argv[i], "-ports")) snprintf(ports, sizeof ports, "%s", argv[i + 1]);
  }
  for (char *p = strtok(ports, ","); p && np < 8; p = strtok(NULL, ",")) pl[np++] = atoi(p);
  struct rlimit rl = {n + 1000, n + 1000}; setrlimit(RLIMIT_NOFILE, &rl);
  signal(SIGPIPE, SIG_IGN); setvbuf(stdout, 0, _IOLBF, 0);
  cs = calloc(n, sizeof(conn_t));
  int ep = epoll_create1(0);
  static struct epoll_event evs[8192];
  double t0 = now(), lastp = t0; long started = 0; char buf[65536]; long scan = 0;
  for (;;) {
    double t = now();
    if (t - t0 > secs) break;
    long want = (long)((t - t0) * rate); if (want > n) want = n;
    while (started < want) { long k = started++; openc(ep, k); cs[k].next = 0; }
    int m = epoll_wait(ep, evs, 8192, 5);
    t = now();
    for (int i = 0; i < m; i++) {
      long k = (long)evs[i].data.u64; conn_t *c = &cs[k];
      if (c->st == 0) continue;
      uint32_t ev = evs[i].events;
      if (c->st == 1 && (ev & (EPOLLOUT | EPOLLERR | EPOLLHUP))) {
        int err = 0; socklen_t el = 4; getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err || (ev & (EPOLLERR | EPOLLHUP))) { wfail++; tfail++; closec(c); continue; }
        struct epoll_event e = {.events = EPOLLIN | EPOLLRDHUP, .data.u64 = (uint64_t)k};
        epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &e);
        sendreq(c);
        continue;
      }
      if (ev & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
        ssize_t r = recv(c->fd, buf, sizeof buf, 0);
        if (r <= 0) { if (r == 0 || errno != EAGAIN) { if (c->st == 2) { wfail++; tfail++; } closec(c); } continue; }
        if (c->st == 2) {
          double ms = (t - c->sent) * 1000; int b = bucket(ms); hwin[b]++; wresp++; if (t - t0 > warm) { htot[b]++; tresp++; }
          if (!(r >= 12 && !memcmp(buf, "HTTP/1.1 200", 12))) { wfail++; if (t - t0 > warm) tfail++; }
          c->st = 3;
          c->next = t + kint * (0.5 + (double)(rnd() % 1000) / 1000.0);
        }
      }
    }
    // ~2% of the table per pass: due requests, timeouts, reconnects
    long per = n / 50 + 1;
    for (long j = 0; j < per && started > 0; j++, scan = (scan + 1) % started) {
      conn_t *c = &cs[scan];
      if (c->st == 3 && t >= c->next) sendreq(c);
      else if (c->st == 2 && t - c->sent > tmo) { wto++; if (t - t0 > warm) tto++; closec(c); }
      else if (c->st == 0 && c->next > 0 && t >= c->next) openc(ep, scan);
    }
    if (t - lastp >= 10) {
      printf("t=%.0fs open=%ld conns=%ld req=%ld resp=%ld fail=%ld to=%ld p50=%.1f p95=%.1f p99=%.1f p999=%.1f ms\n", t - t0, nopen, nconn,
             wreq, wresp, wfail, wto, pct(hwin, .5), pct(hwin, .95), pct(hwin, .99), pct(hwin, .999));
      memset(hwin, 0, sizeof hwin); wreq = wresp = wfail = wto = 0; lastp = t;
    }
  }
  printf("TOTAL req=%ld resp=%ld fail=%ld to=%ld p50=%.1f p95=%.1f p99=%.1f p999=%.1f ms conns=%ld\n", treq, tresp, tfail, tto,
         pct(htot, .5), pct(htot, .95), pct(htot, .99), pct(htot, .999), nconn);
  return 0;
}
